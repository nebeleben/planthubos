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
 */
#include "zigbee.h"

#if CONFIG_PLANTHUB_ZB_ENABLED

#include "esp_zigbee_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* Records the network's channel/PAN and marks it formed. Called once a
 * network actually exists: after BDB formation completes on first start,
 * and immediately on a reboot restore (the stack has already loaded
 * zb_storage by the time ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT fires, so these
 * getters are valid right away). */
static void record_net_info(void)
{
    uint8_t channel = esp_zb_get_current_channel();
    uint16_t pan_id = esp_zb_get_pan_id();
    portENTER_CRITICAL(&s_mux);
    s_channel = channel;
    s_pan_id = pan_id;
    s_formed = true;
    portEXIT_CRITICAL(&s_mux);
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

    switch (sig_type) {
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        /* Factory-new stack, nothing in zb_storage: form a brand new
         * network. This is the ONLY path that is allowed to form -- see
         * the REBOOT case below for why forming there would be wrong. */
        ESP_LOGI(TAG, "first start (status %s); forming a new network",
                 esp_err_to_name(err_status));
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        /* zb_storage already has a network: restore it, do NOT form.
         * Forming here would issue a new PAN id and network key and
         * silently orphan every device paired against the old one --
         * exactly the failure mode the M6b spec calls out by name. */
        ESP_LOGI(TAG, "reboot (status %s); restoring network from zb_storage",
                 esp_err_to_name(err_status));
        if (err_status == ESP_OK) {
            record_net_info();
            ESP_LOGI(TAG, "restored on channel %u, PAN 0x%04x", s_channel, s_pan_id);
        } else {
            ESP_LOGE(TAG, "zb_storage restore failed (%s); coordinator has no network",
                     esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        /* Completion of the formation kicked off above. This is the point
         * a first-start coordinator's network actually starts existing --
         * zigbee_net_info()'s *formed only becomes true here or on a
         * successful REBOOT restore above. */
        if (err_status == ESP_OK) {
            record_net_info();
            ESP_LOGI(TAG, "network formed on channel %u, PAN 0x%04x", s_channel, s_pan_id);
        } else {
            ESP_LOGE(TAG, "network formation failed (%s)", esp_err_to_name(err_status));
        }
        break;

    default:
        ESP_LOGD(TAG, "unhandled signal 0x%x (status %s)", (unsigned)sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

/* The stack task. Owns esp_zb_stack_main_loop() for the life of the
 * device -- it never returns on success. Any failure before that loop
 * starts is logged and the task exits; it must NOT call ESP_ERROR_CHECK/
 * abort(), because a coordinator that cannot start must leave the rest of
 * the hub (BLE, web UI) running, not crash the whole device. */
static void zb_task(void *arg)
{
    (void)arg;

    esp_zb_platform_config_t platform_cfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    esp_err_t err = esp_zb_platform_config(&platform_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_platform_config failed (%s); coordinator will not start",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = { .max_children = 10 },
    };
    esp_zb_init(&zb_cfg);

    err = esp_zb_set_primary_network_channel_set(ZB_CHANNEL_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_set_primary_network_channel_set failed (%s); "
                      "coordinator will not start", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* autostart=false: esp_zb_app_signal_handler() above decides whether to
     * form or restore before the stack actually starts running commands,
     * per esp_zb_start()'s own doc comment (no_autostart mode). */
    err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_start failed (%s); coordinator will not start",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_zb_stack_main_loop(); /* infinite; does not return */
}

esp_err_t zigbee_start(void)
{
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

    esp_err_t err = esp_zb_bdb_open_network(CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S);
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
