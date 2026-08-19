/* zigbee.c -- Zigbee coordinator bring-up (M6b spec section 3, Task 1).
 *
 * This task only stands the stack up: one FreeRTOS task that owns
 * esp_zb_stack_main_loop() for the life of the device, a signal handler
 * that tells first-start (form a new network) apart from reboot (restore
 * the one already in zb_storage), and the net-info/permit-join accessors
 * the UI polls. zb_map.c/zb_store.c/zb_interview.c/zb_cmd.c are stubs here
 * -- later tasks replace them wholesale to interview joined devices,
 * persist them, and map their clusters onto PlantHub's capability model.
 * None of that exists yet, so there is nothing for the signal handler to
 * drive beyond forming/restoring the network itself.
 *
 * ORDERING IS LOAD-BEARING (fix round 4). The bring-up below deliberately
 * mirrors ESP-IDF's own esp_zigbee_gateway example step for step:
 * examples/zigbee/esp_zigbee_gateway/main/esp_zigbee_gateway.c. Rounds 1-3
 * shipped a shortened sequence -- no endpoint registered, no reply to
 * ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP, and no coexistence call -- and the hub
 * stopped reaching the LAN entirely. Every deviation from that example was
 * a bug; do not shorten this again without checking it against the example
 * first.
 */
#include "zigbee.h"

#if CONFIG_PLANTHUB_ZB_ENABLED

/* FreeRTOS first: esp_zigbee_core.h declares esp_zb_lock_acquire() in terms
 * of TickType_t, and this file needs pdMS_TO_TICKS for it. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "zigbee";

/* The four 802.15.4 channels that sit in the gaps between the commonly-used
 * 2.4 GHz WiFi channels (1/6/11 and their neighbours) -- restricting the
 * coordinator to this set rather than the library's full 11-26 sweep is
 * what keeps a colocated WiFi radio clean. Proven, not assumed: the M0
 * coex spike ran WiFi ch1 against Zigbee ch15 (a member of this set) for
 * ten minutes with no adv-rate collapse, no WiFi disconnects and no Zigbee
 * retransmit storm (docs/superpowers/specs/2026-08-15-c5-coex-findings.md).
 */
#define ZB_CHANNEL_MASK ((1UL << 15) | (1UL << 20) | (1UL << 25) | (1UL << 26))

/* The coordinator's own endpoint. Zigbee reserves endpoint 0 for the ZDO,
 * so an application endpoint starts at 1. Its cluster set is the minimum a
 * coordinator must expose: Basic (who am I) and Identify (blink on
 * request). Later tasks add clusters here; they do not need a second
 * endpoint. */
#define ZB_ENDPOINT           1
#define ZB_MAX_CHILDREN       10

/* How long zigbee_permit_join() will wait for the stack lock. The stack is
 * documented as not thread-safe: every SDK call from outside a stack
 * callback must hold this lock (esp_zigbee_core.h on esp_zb_lock_acquire).
 * zigbee_permit_join() is called from a webserver task, which is exactly
 * such an outside caller. 200 ms is generous for a lock the stack task only
 * holds for the length of one scheduler iteration, and bounded so a wedged
 * stack cannot block the HTTP handler forever. */
#define ZB_LOCK_WAIT_MS       200

/* Guards every field below. These are a handful of scalars read/written as
 * a quick copy in/out from the stack task, the boot task (zigbee_start())
 * and, once the UI polls it, whichever webserver task calls
 * zigbee_net_info() -- exactly the case a spinlock (portMUX_TYPE) suits,
 * not a SemaphoreHandle_t (see ble_collector.c's s_adv_mux for the same
 * reasoning). It also statically initializes to "unlocked", so there is no
 * window where a caller could see an unconstructed mutex: webserver_start()
 * runs before zigbee_start() in main.c, so zigbee_net_info() must be safe
 * to call before the stack task exists at all. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool     s_started;                 /* zigbee_start() created the task */
static bool     s_formed;                  /* a network exists (formed or restored) */
static uint8_t  s_channel;
static uint16_t s_pan_id;
static int64_t  s_permit_join_deadline_us; /* 0 == window closed */

/* Records the network's channel/PAN and marks it formed. Called from the
 * signal handler only, so the SDK getters below are already on the stack
 * task and need no lock. Returns the values it stored so the caller can log
 * them without a second unguarded read of the file statics. */
static void record_net_info(uint8_t *out_channel, uint16_t *out_pan_id)
{
    uint8_t channel = esp_zb_get_current_channel();
    uint16_t pan_id = esp_zb_get_pan_id();
    portENTER_CRITICAL(&s_mux);
    s_channel = channel;
    s_pan_id = pan_id;
    s_formed = true;
    portEXIT_CRITICAL(&s_mux);
    if (out_channel) *out_channel = channel;
    if (out_pan_id) *out_pan_id = pan_id;
}

/* Required by the esp-zigbee-lib SDK: every signal the stack raises (BDB
 * commissioning progress, ZDO events, ...) arrives here. Only the signals
 * this task needs are handled; everything else is logged at DEBUG and
 * dropped -- later tasks (join/interview handling, Task 6) add cases here
 * rather than replacing this function. */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;
    uint8_t channel = 0;
    uint16_t pan_id = 0;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* esp_zb_start(false) starts the scheduler and buffer pool but
         * stops here and waits for the application: nothing above the MAC
         * is up yet, and no BDB step will ever run unless we ask for one.
         * Rounds 1-3 did not handle this signal at all, so the stack
         * parked here forever -- FIRST_START/REBOOT never fired, the
         * coordinator never formed, and the 802.15.4 radio sat powered
         * with no arbitration against the WiFi and BLE radios sharing the
         * same PHY. That is the window in which this hub stopped reaching
         * the LAN.
         *
         * Two things must happen here, in this order, and this is exactly
         * what the esp_zigbee_gateway example does:
         *
         * 1. Arm WiFi/802.15.4 coexistence. CONFIG_ESP_COEX_SW_COEXIST_ENABLE
         *    only compiles the arbiter in; esp_coex_wifi_i154_enable() is
         *    what actually turns it on for this radio pair (esp_coexist.h,
         *    and esp_coex/src/coexist.c: coex_enable() plus
         *    esp_coex_ieee802154_status_enable()). Without it the two
         *    radios are not arbitrated at all. This hub also runs BLE, so
         *    it is the worst case for leaving that off.
         * 2. Ask BDB to initialize. Only then does the stack load NVRAM and
         *    raise FIRST_START or REBOOT below. */
/* This guard is copied verbatim from esp_coexist.h's own guard around the
 * declaration -- narrowing it to CONFIG_ESP_COEX_SW_COEXIST_ENABLE alone
 * would not compile on a target without an 802.15.4 radio. */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
        {
            esp_err_t coex_err = esp_coex_wifi_i154_enable();
            if (coex_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_coex_wifi_i154_enable failed (%s); WiFi and "
                              "802.15.4 will contend unarbitrated",
                         esp_err_to_name(coex_err));
            } else {
                ESP_LOGI(TAG, "WiFi/802.15.4 coexistence enabled");
            }
        }
#else
        ESP_LOGW(TAG, "built without CONFIG_ESP_COEX_SW_COEXIST_ENABLE; WiFi and "
                      "802.15.4 will contend unarbitrated");
#endif
        ESP_LOGI(TAG, "stack framework up; starting BDB initialization");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        /* Both signals mean the same thing to us -- BDB initialization
         * finished -- and neither one is a reliable statement about whether
         * a network already exists. esp_zb_bdb_is_factory_new() is, and it
         * is what the example branches on. That distinction is the one that
         * keeps paired devices alive: forming a second time would issue a
         * new PAN id and network key and silently orphan every device
         * paired against the old one. So: form ONLY when the stack itself
         * reports the zb_storage partition is factory-new; otherwise the
         * network has been restored from it and there is nothing to do but
         * record what was restored. */
        if (err_status != ESP_OK) {
            ESP_LOGE(TAG, "BDB initialization failed (%s); coordinator has no network",
                     esp_err_to_name(err_status));
            break;
        }
        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "factory-new stack; forming a new network");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            record_net_info(&channel, &pan_id);
            ESP_LOGI(TAG, "network restored from zb_storage on channel %u, PAN 0x%04x",
                     channel, pan_id);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        /* Completion of the formation kicked off above. This is the point
         * a first-start coordinator's network actually starts existing --
         * zigbee_net_info()'s *formed only becomes true here or on the
         * restore path above. */
        if (err_status == ESP_OK) {
            record_net_info(&channel, &pan_id);
            ESP_LOGI(TAG, "network formed on channel %u, PAN 0x%04x", channel, pan_id);
        } else {
            ESP_LOGE(TAG, "network formation failed (%s)", esp_err_to_name(err_status));
        }
        break;

    default:
        ESP_LOGD(TAG, "unhandled signal %s (0x%x), status %s",
                 esp_zb_zdo_signal_to_string(sig_type), (unsigned)sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

/* Builds the coordinator's one endpoint. esp_zb_start() requires a
 * registered ZCL device context -- the example registers one before
 * starting and so must we; rounds 1-3 skipped this entirely. Returns NULL
 * on allocation failure, which the caller treats as "no coordinator" rather
 * than as a fatal error. */
static esp_zb_ep_list_t *build_endpoint(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    if (!ep_list) return NULL;

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    if (!cluster_list) return NULL;

    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(NULL);
    if (!basic || !identify) return NULL;

    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(cluster_list, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ZB_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_REMOTE_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };
    if (esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, ep_cfg) != ESP_OK) return NULL;
    return ep_list;
}

/* The stack task. Owns esp_zb_stack_main_loop() for the life of the
 * device -- it never returns on success. Any failure before that loop
 * starts is logged and the task exits; it must NOT call ESP_ERROR_CHECK/
 * abort(), because a coordinator that cannot start must leave the rest of
 * the hub (BLE, web UI) running, not crash the whole device. */
static void zb_task(void *arg)
{
    (void)arg;

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = { .max_children = ZB_MAX_CHILDREN },
    };
    esp_zb_init(&zb_cfg);

    esp_err_t err = esp_zb_set_primary_network_channel_set(ZB_CHANNEL_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_set_primary_network_channel_set failed (%s); "
                      "coordinator will not start", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_zb_ep_list_t *ep_list = build_endpoint();
    if (!ep_list) {
        ESP_LOGE(TAG, "could not build the coordinator endpoint; coordinator will not start");
        vTaskDelete(NULL);
        return;
    }
    err = esp_zb_device_register(ep_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_device_register failed (%s); coordinator will not start",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* autostart=false: the stack comes up as far as its scheduler and
     * buffer pool, then raises ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP and waits.
     * The handler above is what drives it onwards -- see that case for why
     * ignoring the signal is what broke rounds 1-3. */
    err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_start failed (%s); coordinator will not start",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_zb_stack_main_loop(); /* infinite; does not return in normal operation */
    ESP_LOGE(TAG, "the Zigbee stack main loop returned; coordinator is down");
    vTaskDelete(NULL);
}

esp_err_t zigbee_start(void)
{
    /* esp_zb_platform_config() runs HERE, on the caller's task, and not
     * inside zb_task -- this is where the example calls it (in app_main,
     * before the stack task exists) and the ordering matters: it is what
     * selects the native 15.4 radio and installs the platform's serial
     * configuration, and esp_zb_init() reads what it stored. Doing it here
     * also means a platform-config failure is reported synchronously to
     * app_main instead of vanishing into a task that silently deletes
     * itself. */
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    esp_err_t err = esp_zb_platform_config(&platform_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_platform_config failed (%s); running without Zigbee",
                 esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(zb_task, "zigbee_stack", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(zigbee_stack) failed; running without Zigbee");
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_mux);
    s_started = true;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool zigbee_net_info(uint8_t *channel, uint16_t *pan_id, bool *formed)
{
    bool started;
    portENTER_CRITICAL(&s_mux);
    started = s_started;
    if (channel) *channel = s_channel;
    if (pan_id) *pan_id = s_pan_id;
    if (formed) *formed = s_formed;
    portEXIT_CRITICAL(&s_mux);
    return started;
}

bool zigbee_permit_join(void)
{
    bool formed;
    portENTER_CRITICAL(&s_mux);
    formed = s_formed;
    portEXIT_CRITICAL(&s_mux);
    if (!formed) return false;

    /* This runs on a webserver task, not on the stack task, so it is an
     * "outside" caller and must hold the stack lock for the duration of the
     * SDK call. Rounds 1-3 called esp_zb_bdb_open_network() bare from here,
     * which races the stack task inside ZBOSS's own scheduler. */
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(ZB_LOCK_WAIT_MS))) {
        ESP_LOGE(TAG, "could not acquire the Zigbee stack lock; permit-join not opened");
        return false;
    }
    esp_err_t err = esp_zb_bdb_open_network(CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S);
    esp_zb_lock_release();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_bdb_open_network failed (%s)", esp_err_to_name(err));
        return false;
    }

    /* Closes on expiry (zigbee_permit_join_remaining() below) and on
     * reboot (this deadline lives in RAM only, so a reboot resets it to
     * closed for free). Closing on a successful join is Task 6's job --
     * it owns the device-announce signal that a join actually arrives on,
     * which does not exist yet at Task 1. */
    portENTER_CRITICAL(&s_mux);
    s_permit_join_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S * 1000000;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

uint8_t zigbee_permit_join_remaining(void)
{
    int64_t deadline;
    portENTER_CRITICAL(&s_mux);
    deadline = s_permit_join_deadline_us;
    portEXIT_CRITICAL(&s_mux);

    if (deadline == 0) return 0;
    int64_t now = esp_timer_get_time();
    if (now >= deadline) return 0;
    /* CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S's Kconfig range tops out at 254, so
     * this always fits uint8_t without needing to clamp. */
    return (uint8_t)((deadline - now) / 1000000);
}

#else /* !CONFIG_PLANTHUB_ZB_ENABLED */

esp_err_t zigbee_start(void)
{
    return ESP_OK;
}

bool zigbee_net_info(uint8_t *channel, uint16_t *pan_id, bool *formed)
{
    (void)channel;
    (void)pan_id;
    (void)formed;
    return false;
}

bool zigbee_permit_join(void)
{
    return false;
}

uint8_t zigbee_permit_join_remaining(void)
{
    return 0;
}

#endif /* CONFIG_PLANTHUB_ZB_ENABLED */
