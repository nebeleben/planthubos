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
#include "freertos/semphr.h"
#include "esp_zigbee_core.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Task 6: the joined-device store, the interview it drives, and the
 * registry/actor-table it populates. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "zb_store.h"
#include "zb_interview.h"
#include "zb_map.h"
#include "capability.h"
#include "data_core.h"
#include "actor.h"

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

/* ---------------------------------------------------------------------
 * Task 6: the joined-device table on flash, and the interview that fills
 * it in. zb_map.c/zb_store.c/zb_interview.c (Tasks 2-4) are the pure
 * modules this section drives; everything below is the impure half --
 * file I/O, the ZDO/ZCL requests and callbacks, and registry/actor-table
 * population -- that this file's own header comment (Task 1) said later
 * tasks would add.
 * --------------------------------------------------------------------- */

#define ZB_STORE_PATH     "/storage/zb_devices.bin"
#define ZB_STORE_TMP_PATH "/storage/zb_devices.tmp"

/* One tick per second is plenty against a 30 s interview timeout
 * (ZB_IV_TIMEOUT_S); the config-report cursor does not wait on a tick at
 * all -- see zb_iv_pump() below. */
#define ZB_IV_TICK_MS 1000

/* s_store is read by zigbee_device_list()/zigbee_device_rename()/
 * zigbee_device_remove() (a webserver task) as well as written by the
 * interview (the stack task), so -- unlike s_mux above -- it needs a real
 * FreeRTOS mutex: the store's save path does LittleFS I/O, which must not
 * run inside a portMUX critical section (same reasoning as actor.c's
 * s_lock, actor_lock()/actor_unlock()). Statically allocated, so there is
 * no window where a caller could see it unconstructed. */
static SemaphoreHandle_t s_store_mutex;
static StaticSemaphore_t s_store_mutex_buf;
static zb_table_t        s_store;

/* The interview state machine and its single in-flight slot. Touched only
 * from the stack task -- the signal handler, the ZDO/ZCL response
 * callbacks and the scheduler-alarm tick below all run there (see
 * record_net_info()'s own comment for the same claim about the SDK
 * getters) -- so, unlike s_store, none of this needs a lock. */
static zb_iv_t  s_iv;
static bool     s_iv_active;
static bool     s_iv_ticking;
/* Bumped every zb_interview_begin(): the generation a ZDO request was sent
 * under, carried back in the response's user_ctx, so a reply that finally
 * arrives after ITS interview has already timed out -- and the next
 * queued device's interview has since begun -- is recognised as stale and
 * dropped instead of mutating whatever interview is running now. */
static uint32_t s_iv_generation;

typedef struct {
    uint8_t  eui64[8];
    uint16_t short_addr;
} zb_join_t;

/* "Only one interview runs at a time; a join arriving while one is in
 * flight is queued" (Task 6 brief) -- sized to ZB_STORE_MAX_DEVICES, the
 * same budget the store itself is bound to, rather than an arbitrary
 * smaller number: the queue can never need to hold more joins than the
 * store could ever remember devices. */
static zb_join_t s_iv_queue[ZB_STORE_MAX_DEVICES];
static uint8_t   s_iv_queue_head;
static uint8_t   s_iv_queue_count;

static void zb_iv_service(void);
static void zb_iv_pump(void);
static void zb_iv_tick_cb(uint8_t param);
static void zb_iv_handle_store(void);
static void zb_iv_send_active_ep(void);
static void zb_iv_send_simple_desc(void);
static void zb_iv_send_config_report(void);
static void zb_iv_enqueue(const uint8_t eui64[8], uint16_t short_addr);
static void zb_iv_active_ep_cb(esp_zb_zdp_status_t status, uint8_t ep_count,
                                uint8_t *ep_id_list, void *user_ctx);
static void zb_iv_simple_desc_cb(esp_zb_zdp_status_t status,
                                  esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);

/* Uptime seconds -- the same now_s() convention data_core.c/ble_collector.c
 * already use (esp_timer_get_time() is microseconds since boot). Named
 * zb_now_s(), not now_s(), so it cannot be confused with the many local
 * `uint32_t now_s` variables that convention leaves scattered elsewhere in
 * this codebase. */
static uint32_t zb_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

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

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        /* A device joined (or rejoined) the network and is announcing
         * itself -- Task 6's entry point into the interview, and also the
         * "successful join" Task 1's permit-join comment deferred to this
         * task: the window must close now, not just on expiry, so it does
         * not stay open to any other device in radio range for the rest
         * of its 180 s. */
        esp_zb_zdo_signal_device_annce_params_t *annce =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (!annce) {
            ESP_LOGW(TAG, "device-announce signal carried no parameters; ignored");
            break;
        }
        ESP_LOGI(TAG, "device announced: short 0x%04x", annce->device_short_addr);

        bool window_was_open;
        portENTER_CRITICAL(&s_mux);
        window_was_open = s_permit_join_deadline_us != 0;
        s_permit_join_deadline_us = 0;
        portEXIT_CRITICAL(&s_mux);
        if (window_was_open) {
            /* Called from the signal handler, so this is already on the
             * stack task -- no esp_zb_lock_acquire() needed, same as
             * record_net_info()'s SDK calls above. */
            esp_err_t close_err = esp_zb_bdb_close_network();
            if (close_err != ESP_OK) {
                ESP_LOGW(TAG, "esp_zb_bdb_close_network failed (%s) after a device "
                              "joined; the network may still accept joins until the "
                              "window expires", esp_err_to_name(close_err));
            } else {
                ESP_LOGI(TAG, "permit-join window closed after a device joined");
            }
        }

        zb_iv_enqueue(annce->ieee_addr, annce->device_short_addr);
        break;
    }

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

/* ---- store I/O (Task 6 step 2: tmp+rename, mirroring actor_persist.c;
 * this adds an explicit fsync() that the reviewed actor_persist.c does
 * not, because the Task 6 brief calls for one). Every call site below
 * holds s_store_mutex already. ---- */

static void zb_store_load(void)
{
    zb_store_init(&s_store);

    uint8_t buf[ZB_STORE_IMAGE_MAX];
    FILE *f = fopen(ZB_STORE_PATH, "rb");
    if (!f) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "%s: open for read failed (errno=%d); starting this boot with "
                          "no known Zigbee devices", ZB_STORE_PATH, errno);
        } else {
            ESP_LOGI(TAG, "%s: not present (first boot, or no Zigbee device has joined "
                          "yet)", ZB_STORE_PATH);
        }
        return;
    }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (!zb_store_deserialize(&s_store, buf, n)) {
        ESP_LOGW(TAG, "%s: %u byte(s) unreadable (bad magic, version, length or count); "
                      "starting this boot with no known Zigbee devices",
                 ZB_STORE_PATH, (unsigned)n);
        zb_store_init(&s_store);
        return;
    }
    ESP_LOGI(TAG, "%u Zigbee device(s) restored from before this boot",
             (unsigned)s_store.count);
}

/* Caller must hold s_store_mutex. */
static void zb_store_save(void)
{
    if (s_store.count == 0) {
        /* Nothing to remember; remove()'s only failure here is ENOENT. */
        remove(ZB_STORE_PATH);
        return;
    }

    uint8_t buf[ZB_STORE_IMAGE_MAX];
    size_t len = zb_store_serialize(&s_store, buf, sizeof buf);
    if (len == 0) {
        ESP_LOGE(TAG, "serialize failed for %u Zigbee device(s); not persisted",
                 (unsigned)s_store.count);
        return;
    }

    /* tmp + rename, the discipline actor_persist.c uses (see that file's
     * own comment): rename() is atomic on LittleFS, so a power loss leaves
     * either the old complete file or the new one, never a half-written
     * device table. */
    FILE *f = fopen(ZB_STORE_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for write (errno=%d); a reboot now would lose "
                      "%u Zigbee device(s)", ZB_STORE_TMP_PATH, errno,
                 (unsigned)s_store.count);
        return;
    }
    size_t wrote = fwrite(buf, 1, len, f);
    if (wrote != len) {
        ESP_LOGW(TAG, "short write to %s persisting %u Zigbee device(s)",
                 ZB_STORE_TMP_PATH, (unsigned)s_store.count);
        fclose(f);
        remove(ZB_STORE_TMP_PATH);
        return;
    }
    if (fflush(f) != 0) {
        ESP_LOGW(TAG, "fflush(%s) failed (errno=%d)", ZB_STORE_TMP_PATH, errno);
        fclose(f);
        remove(ZB_STORE_TMP_PATH);
        return;
    }
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync(%s) failed (errno=%d)", ZB_STORE_TMP_PATH, errno);
        fclose(f);
        remove(ZB_STORE_TMP_PATH);
        return;
    }
    if (fclose(f) != 0) {
        ESP_LOGW(TAG, "fclose(%s) failed (errno=%d)", ZB_STORE_TMP_PATH, errno);
        remove(ZB_STORE_TMP_PATH);
        return;
    }
    if (rename(ZB_STORE_TMP_PATH, ZB_STORE_PATH) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno=%d); a reboot now would lose %u "
                      "Zigbee device(s)", ZB_STORE_TMP_PATH, ZB_STORE_PATH, errno,
                 (unsigned)s_store.count);
        remove(ZB_STORE_TMP_PATH);
    }
}

/* Caller must hold s_store_mutex. */
static int zb_store_find_by_short(uint16_t short_addr)
{
    for (int i = 0; i < s_store.count; i++) {
        if (s_store.dev[i].short_addr == short_addr) return i;
    }
    return -1;
}

/* Registers every ALREADY-INTERVIEWED device the store restored from flash.
 * The registry and the actor table are both RAM-only and start empty every
 * boot, so they have no memory of a device this hub already knows from
 * /storage/zb_devices.bin -- unlike the store itself, which just came back
 * under zb_store_load() above.
 *
 * A device restored with interviewed == 0 is skipped here, for the same
 * reason zb_iv_handle_store() below skips one live: it never answered, so
 * it has no capability or action ids to register, and giving it a registry
 * entry now that a live failure would not have given it would be an
 * inconsistency this codebase's own "same identity, same rules" discipline
 * (zb_store.h's EUI-64 keying comment) argues against. It stays visible via
 * zigbee_device_list(), as joined-but-not-interviewed. */
static void zb_register_restored_devices(void)
{
    uint32_t t = zb_now_s();
    for (int i = 0; i < s_store.count; i++) {
        const zb_device_t *dev = &s_store.dev[i];
        if (!dev->interviewed) continue;

        device_id_t id = { .kind = DEV_KIND_ZIGBEE };
        memcpy(id.addr, dev->eui64, 8);
        int dev_idx = data_core_find_or_create_index(&id, t);
        if (dev_idx < 0) {
            ESP_LOGW(TAG, "registry full; a restored Zigbee device has no registry entry "
                          "this boot");
            continue;
        }
        for (uint8_t a = 0; a < dev->action_count; a++) {
            /* param_max=0, no flags: every action zb_map.c hands out today
             * (On/Off) takes no parameter -- zb_map.h's own comment. */
            if (!actor_declare(dev_idx, dev->actions[a], 0, 0)) {
                ESP_LOGW(TAG, "device %d: could not re-declare action %u after restore",
                         dev_idx, (unsigned)dev->actions[a]);
            }
        }
    }
}

/* ---- ZCL attribute report -> capability value (Task 6 step 5) ---- */

/* Attribute wire type to declare in a Configure Reporting request, per
 * cluster. zb_map.h is deliberately silent on wire types (it is about
 * VALUES, not encoding -- see its own header comment), so this mirrors
 * zb_map.c's cluster table by hand; keep the two in sync if a cluster is
 * ever added there. */
static uint8_t zb_report_attr_type(uint16_t cluster)
{
    switch (cluster) {
    case 0x0402: /* Temperature Measurement: MeasuredValue, int16 */
    case 0x0403: /* Pressure Measurement: MeasuredValue, int16 */
        return ESP_ZB_ZCL_ATTR_TYPE_S16;
    case 0x0405: /* Relative Humidity: MeasuredValue, uint16 */
    case 0x0400: /* Illuminance Measurement: MeasuredValue, uint16 */
    case 0x0408: /* Soil Moisture: MeasuredValue, uint16 */
        return ESP_ZB_ZCL_ATTR_TYPE_U16;
    case 0x0001: /* Power Configuration: BatteryPercentageRemaining, uint8 */
        return ESP_ZB_ZCL_ATTR_TYPE_U8;
    case 0x0006: /* On/Off: OnOff, bool */
        return ESP_ZB_ZCL_ATTR_TYPE_BOOL;
    default:
        /* Never actually requested -- on_clusters() only queues a cluster
         * for reporting once zb_map_report_attr() already named a real
         * attribute for it -- but a safe, inert fallback beats an
         * unreachable-looking default. */
        return ESP_ZB_ZCL_ATTR_TYPE_U16;
    }
}

/* Widens a ZCL attribute value to int32_t for zb_map_zcl_to_value(), which
 * -- like the rest of zb_map.h -- works in the capability's own unit space
 * and takes a plain int32_t raw value rather than a typed ZCL blob. Returns
 * false for a width/signedness this milestone's clusters never report,
 * which zb_handle_report_attr() below treats the same as any other
 * unmapped/sentinel value: dropped, not substituted. */
static bool zcl_attr_to_i32(const esp_zb_zcl_attribute_data_t *data, int32_t *out)
{
    if (!data->value) return false;
    switch (data->type) {
    case ESP_ZB_ZCL_ATTR_TYPE_U8:
    case ESP_ZB_ZCL_ATTR_TYPE_8BIT:
    case ESP_ZB_ZCL_ATTR_TYPE_BOOL:
    case ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM:
        *out = *(const uint8_t *)data->value;
        return true;
    case ESP_ZB_ZCL_ATTR_TYPE_S8:
        *out = *(const int8_t *)data->value;
        return true;
    case ESP_ZB_ZCL_ATTR_TYPE_U16:
    case ESP_ZB_ZCL_ATTR_TYPE_16BIT:
    case ESP_ZB_ZCL_ATTR_TYPE_16BIT_ENUM:
        *out = *(const uint16_t *)data->value;
        return true;
    case ESP_ZB_ZCL_ATTR_TYPE_S16:
        *out = *(const int16_t *)data->value;
        return true;
    default:
        return false;
    }
}

/* ESP_ZB_CORE_REPORT_ATTR_CB_ID handler (registered in zb_task() below).
 * Runs on the stack task, same as every other callback in this file. */
static void zb_handle_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    if (!msg || msg->status != ESP_ZB_ZCL_STATUS_SUCCESS) return;

    uint8_t eui64[8];
    bool found;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    int idx = (msg->src_address.addr_type == ESP_ZB_ZCL_ADDR_TYPE_IEEE)
                  ? zb_store_find(&s_store, msg->src_address.u.ieee_addr)
                  : zb_store_find_by_short(msg->src_address.u.short_addr);
    found = idx >= 0;
    if (found) memcpy(eui64, s_store.dev[idx].eui64, 8);
    xSemaphoreGive(s_store_mutex);

    if (!found) {
        ESP_LOGD(TAG, "attribute report from an unrecognised device; ignored");
        return;
    }

    /* Cluster unmapped, or the value is one of ZCL's not-a-reading
     * sentinels: dropped silently, never substituted -- a fabricated
     * reading in a plant's history is worse than a gap (zb_map.h). */
    uint8_t cap = zb_map_cluster_to_cap(msg->cluster);
    if (cap == ZB_MAP_NONE) return;

    int32_t raw;
    if (!zcl_attr_to_i32(&msg->attribute.data, &raw)) return;

    float value;
    if (!zb_map_zcl_to_value(msg->cluster, raw, &value)) return;

    device_id_t id = { .kind = DEV_KIND_ZIGBEE };
    memcpy(id.addr, eui64, 8);
    data_core_submit_cap_id(&id, cap, value);
}

static esp_err_t zb_core_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                         const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        zb_handle_report_attr((const esp_zb_zcl_report_attr_message_t *)message);
        return ESP_OK;
    default:
        ESP_LOGD(TAG, "unhandled ZCL core action 0x%x", (unsigned)callback_id);
        return ESP_OK;
    }
}

/* ---- the interview driver (Task 6 step 3) ---- */

static void zb_iv_ensure_ticking(void)
{
    if (s_iv_ticking) return;
    s_iv_ticking = true;
    esp_zb_scheduler_alarm(zb_iv_tick_cb, 0, ZB_IV_TICK_MS);
}

static void zb_iv_enqueue(const uint8_t eui64[8], uint16_t short_addr)
{
    if (s_iv_active && memcmp(s_iv.dev.eui64, eui64, 8) == 0) {
        return; /* already interviewing this device */
    }
    for (uint8_t i = 0; i < s_iv_queue_count; i++) {
        uint8_t at = (uint8_t)((s_iv_queue_head + i) % ZB_STORE_MAX_DEVICES);
        if (memcmp(s_iv_queue[at].eui64, eui64, 8) == 0) return; /* already queued */
    }
    if (s_iv_queue_count >= ZB_STORE_MAX_DEVICES) {
        ESP_LOGW(TAG, "interview queue full (%u); dropping a newly joined device's "
                      "announce -- it stays joined but will not be interviewed until it "
                      "announces again", (unsigned)ZB_STORE_MAX_DEVICES);
        return;
    }
    uint8_t at = (uint8_t)((s_iv_queue_head + s_iv_queue_count) % ZB_STORE_MAX_DEVICES);
    memcpy(s_iv_queue[at].eui64, eui64, 8);
    s_iv_queue[at].short_addr = short_addr;
    s_iv_queue_count++;

    zb_iv_ensure_ticking();
    zb_iv_service(); /* start it now rather than waiting up to ZB_IV_TICK_MS */
}

static void zb_iv_tick_cb(uint8_t param)
{
    (void)param;
    zb_iv_service();
    if (s_iv_active || s_iv_queue_count > 0) {
        esp_zb_scheduler_alarm(zb_iv_tick_cb, 0, ZB_IV_TICK_MS);
    } else {
        s_iv_ticking = false;
    }
}

/* Starts the next queued interview if none is running, then drives
 * whichever one is now active forward. Callers: a new join (zb_iv_enqueue),
 * the 1 Hz tick (zb_iv_tick_cb), and every ZDO response callback below --
 * one entry point for "something may have changed, make progress". */
static void zb_iv_service(void)
{
    for (;;) {
        if (!s_iv_active) {
            if (s_iv_queue_count == 0) return;
            uint8_t eui64[8];
            memcpy(eui64, s_iv_queue[s_iv_queue_head].eui64, 8);
            uint16_t short_addr = s_iv_queue[s_iv_queue_head].short_addr;
            s_iv_queue_head = (uint8_t)((s_iv_queue_head + 1) % ZB_STORE_MAX_DEVICES);
            s_iv_queue_count--;

            s_iv_generation++;
            zb_interview_begin(&s_iv, eui64, short_addr, zb_now_s());
            s_iv_active = true;
        }
        zb_iv_pump();
        if (s_iv_active) return; /* waiting on a callback or the next tick */
        /* This interview just finished (ZB_IV_ACT_STORE handled) -- loop
         * to pick up the next queued one, if any, without waiting for a
         * tick. */
    }
}

/* Drives s_iv forward as far as it can go without waiting for anything:
 * every ZB_IV_ACT_SEND_CONFIG_REPORT is sent back-to-back (the state
 * machine does not wait for a reply between them -- see zb_interview.c's
 * ZB_IV_WAIT_CONFIG_REPORT case), while ZB_IV_ACT_SEND_ACTIVE_EP/
 * SIMPLE_DESC each stop and wait for their ZDO response (or the timeout)
 * before this is called again. */
static void zb_iv_pump(void)
{
    for (;;) {
        zb_iv_action_t act = zb_interview_step(&s_iv, zb_now_s());
        switch (act) {
        case ZB_IV_ACT_NONE:
            return;
        case ZB_IV_ACT_SEND_ACTIVE_EP:
            zb_iv_send_active_ep();
            return;
        case ZB_IV_ACT_SEND_SIMPLE_DESC:
            zb_iv_send_simple_desc();
            return;
        case ZB_IV_ACT_SEND_CONFIG_REPORT:
            zb_iv_send_config_report();
            continue;
        case ZB_IV_ACT_STORE:
            zb_iv_handle_store();
            s_iv_active = false;
            return;
        }
    }
}

static void zb_iv_active_ep_cb(esp_zb_zdp_status_t status, uint8_t ep_count,
                                uint8_t *ep_id_list, void *user_ctx)
{
    if (!s_iv_active || (uint32_t)(uintptr_t)user_ctx != s_iv_generation) {
        return; /* a reply for an interview that has already moved on */
    }
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "active-endpoint request failed (status 0x%02x); the interview will "
                      "time out if the device never answers", (unsigned)status);
        return; /* no progress recorded; zb_interview_step()'s own deadline handles it */
    }
    zb_interview_on_endpoints(&s_iv, ep_id_list, ep_count);
    zb_iv_service();
}

static void zb_iv_send_active_ep(void)
{
    esp_zb_zdo_active_ep_req_param_t req = { .addr_of_interest = s_iv.dev.short_addr };
    esp_zb_zdo_active_ep_req(&req, zb_iv_active_ep_cb, (void *)(uintptr_t)s_iv_generation);
}

/* Fix round 1: this was forward-declared (and called from zb_iv_pump() for
 * ZB_IV_ACT_SEND_SIMPLE_DESC) but never defined -- a link error the
 * controller caught. Modeled directly on zb_iv_send_active_ep() above:
 * same request/callback/generation shape, plus the one field that request
 * doesn't need. iv.pending_endpoint -- set by zb_interview_step() just
 * before it returns ZB_IV_ACT_SEND_SIMPLE_DESC -- is which endpoint this
 * asks about; using anything else (e.g. defaulting to 0) would silently
 * interview endpoint 0 forever regardless of what the device actually
 * announced. */
static void zb_iv_send_simple_desc(void)
{
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = s_iv.dev.short_addr,
        .endpoint = s_iv.pending_endpoint,
    };
    esp_zb_zdo_simple_desc_req(&req, zb_iv_simple_desc_cb, (void *)(uintptr_t)s_iv_generation);
}

static void zb_iv_simple_desc_cb(esp_zb_zdp_status_t status,
                                  esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    if (!s_iv_active || (uint32_t)(uintptr_t)user_ctx != s_iv_generation) {
        return;
    }
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGW(TAG, "simple-descriptor request for endpoint %u failed (status 0x%02x)",
                 (unsigned)s_iv.pending_endpoint, (unsigned)status);
        return;
    }
    /* Only input (server-role) clusters carry sensor/actuator semantics for
     * the auto-map -- app_cluster_list[0..input_count) per the struct's own
     * doc comment; output (client-role) clusters are not mapped. Clamped to
     * ZB_IV_MAX_CLUSTERS, which zb_interview.h defines for exactly this: a
     * bound this driver enforces before handing clusters to the pure
     * module (which itself never writes past cap_count/action_count/
     * report_count's own MAX-bounded arrays regardless). */
    uint8_t n = simple_desc->app_input_cluster_count;
    if (n > ZB_IV_MAX_CLUSTERS) n = ZB_IV_MAX_CLUSTERS;
    zb_interview_on_clusters(&s_iv, simple_desc->endpoint, simple_desc->app_cluster_list, n);
    zb_iv_service();
}

static void zb_iv_send_config_report(void)
{
    uint16_t cluster = s_iv.report_clusters[s_iv.report_cursor - 1];
    uint16_t attr = zb_map_report_attr(cluster);
    if (attr == ZB_MAP_NO_ATTR) {
        /* Shouldn't happen -- on_clusters() only ever queues a cluster here
         * once zb_map_report_attr() already named a real attribute for it
         * -- but never send a Configure Reporting command with nothing to
         * name. */
        ESP_LOGW(TAG, "cluster 0x%04x queued for reporting has no mapped attribute; "
                      "skipped", cluster);
        return;
    }

    uint8_t attr_type = zb_report_attr_type(cluster);
    uint8_t  change_u8  = 1;
    uint16_t change_u16 = 1;
    int16_t  change_s16 = 1;
    void *reportable_change;
    switch (attr_type) {
    case ESP_ZB_ZCL_ATTR_TYPE_U8:  reportable_change = &change_u8;  break;
    case ESP_ZB_ZCL_ATTR_TYPE_U16: reportable_change = &change_u16; break;
    case ESP_ZB_ZCL_ATTR_TYPE_S16: reportable_change = &change_s16; break;
    default:
        /* ZCL forbids a reportable-change field on a discrete/boolean
         * attribute (On/Off's OnOff, here) -- omit it, not just zero it. */
        reportable_change = NULL;
        break;
    }

    esp_zb_zcl_config_report_record_t record = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID = attr,
        .attrType = attr_type,
        .min_interval = 1,
        .max_interval = 3600,
        .reportable_change = reportable_change,
    };
    esp_zb_zcl_config_report_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = s_iv.dev.short_addr,
            .dst_endpoint = s_iv.dev.endpoint,
            .src_endpoint = ZB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .record_number = 1,
        .record_field = &record,
    };
    esp_zb_zcl_config_report_cmd_req(&cmd);
}

/* ZB_IV_ACT_STORE (Task 6 step 4): persist the interview result, then --
 * only when it actually finished -- register it with the registry and
 * actor table. */
static void zb_iv_handle_store(void)
{
    zb_device_t *dev = &s_iv.dev;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    int idx = zb_store_upsert(&s_store, dev);
    if (idx < 0) {
        xSemaphoreGive(s_store_mutex);
        ESP_LOGE(TAG, "device table full (%u); a newly joined device stays on the "
                      "network but the hub cannot remember it",
                 (unsigned)ZB_STORE_MAX_DEVICES);
        return;
    }
    zb_store_save();
    xSemaphoreGive(s_store_mutex);

    if (!dev->interviewed) {
        /* Judgement call (Task 6 brief): still stored, not dropped -- a
         * device joined to the network but absent from the UI is an
         * orphan the user could only recover from with a factory reset.
         * No registry entry, no actions; shown as joined-but-not-
         * interviewed with a retry, via zigbee_device_list(). */
        ESP_LOGW(TAG, "device joined but did not finish its interview; stored without a "
                      "registry entry");
        return;
    }

    device_id_t id = { .kind = DEV_KIND_ZIGBEE };
    memcpy(id.addr, dev->eui64, 8);
    int dev_idx = data_core_find_or_create_index(&id, zb_now_s());
    if (dev_idx < 0) {
        char idbuf[24];
        ESP_LOGW(TAG, "registry full; interviewed device %s has no registry entry",
                 device_id_format(&id, idbuf, sizeof idbuf));
        return;
    }
    for (uint8_t a = 0; a < dev->action_count; a++) {
        /* param_max=0, no flags: every action zb_map.c hands out today
         * (On/Off) takes no parameter -- zb_map.h's own comment. */
        if (!actor_declare(dev_idx, dev->actions[a], 0, 0)) {
            ESP_LOGW(TAG, "device %d: could not declare action %u", dev_idx,
                     (unsigned)dev->actions[a]);
        }
    }
    ESP_LOGI(TAG, "device %d interviewed: %u capability(ies), %u action(s)", dev_idx,
             (unsigned)dev->cap_count, (unsigned)dev->action_count);
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

    /* Task 6: the ZCL side of the interview (Configure Reporting) and of
     * ongoing operation (the attribute reports it configures) both arrive
     * through this one callback -- see zb_core_action_handler()/
     * zb_handle_report_attr() above. Registered before esp_zb_start()
     * below, so no report can arrive before a handler exists for it. */
    esp_zb_core_action_handler_register(zb_core_action_handler);

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

    /* Task 6: load what a previous boot learned about joined devices, and
     * re-register the interviewed ones with the registry/actor table --
     * both RAM-only and empty every boot, so neither remembers a device
     * this hub already knows from /storage/zb_devices.bin -- BEFORE the
     * stack task (and so any join) can run. Doing this here, on the
     * caller's task, rather than inside zb_task means zigbee_device_list()
     * sees real data the moment zigbee_start() returns, not a race against
     * the stack task's first tick. */
    s_store_mutex = xSemaphoreCreateMutexStatic(&s_store_mutex_buf);
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    zb_store_load();
    xSemaphoreGive(s_store_mutex);
    zb_register_restored_devices();

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

    /* Closes on expiry (zigbee_permit_join_remaining() below), on reboot
     * (this deadline lives in RAM only, so a reboot resets it to closed
     * for free), and -- Task 6 -- the moment a device actually joins: see
     * esp_zb_app_signal_handler()'s ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE case
     * above, which closes it via esp_zb_bdb_close_network() as soon as the
     * device-announce signal fires, rather than waiting for that device's
     * interview to finish (or fail). The window's job is done once
     * SOMEONE has joined; every second it stays open after that is a
     * second some other device in radio range could join too. */
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

/* Task 6 step 6: list, rename, remove. All three check s_started first,
 * the same guard zigbee_net_info() uses -- s_store_mutex is created and
 * the store loaded inside zigbee_start() (above), before s_started is set
 * true, so "started" here also means "s_store_mutex exists and s_store
 * reflects flash", not just "the stack task was created". */

int zigbee_device_list(zb_device_t *out, size_t max)
{
    bool started;
    portENTER_CRITICAL(&s_mux);
    started = s_started;
    portEXIT_CRITICAL(&s_mux);
    if (!started || !out || max == 0) return 0;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    size_t n = (size_t)s_store.count;
    if (n > max) n = max;
    memcpy(out, s_store.dev, n * sizeof(zb_device_t));
    xSemaphoreGive(s_store_mutex);
    return (int)n;
}

bool zigbee_device_rename(const uint8_t eui64[8], const char *name)
{
    bool started;
    portENTER_CRITICAL(&s_mux);
    started = s_started;
    portEXIT_CRITICAL(&s_mux);
    if (!started || !eui64 || !name) return false;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    bool ok = false;
    int idx = zb_store_find(&s_store, eui64);
    if (idx >= 0) {
        zb_device_t d = s_store.dev[idx];
        strncpy(d.name, name, ZB_STORE_NAME_MAX - 1);
        d.name[ZB_STORE_NAME_MAX - 1] = '\0';
        /* Same EUI-64: zb_store_upsert() replaces this entry in place
         * rather than appending a second one. */
        ok = zb_store_upsert(&s_store, &d) >= 0;
        if (ok) zb_store_save();
    }
    xSemaphoreGive(s_store_mutex);
    return ok;
}

bool zigbee_device_remove(const uint8_t eui64[8])
{
    bool started;
    portENTER_CRITICAL(&s_mux);
    started = s_started;
    portEXIT_CRITICAL(&s_mux);
    if (!started || !eui64) return false;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    int idx = zb_store_find(&s_store, eui64);
    uint16_t short_addr = (idx >= 0) ? s_store.dev[idx].short_addr : 0;
    bool removed = zb_store_remove(&s_store, eui64);
    if (removed) zb_store_save();
    xSemaphoreGive(s_store_mutex);
    if (!removed) return false;

    /* registry.h has no delete (Task 6 brief): the registry entry this
     * device may have had -- its capability readings, and, if it was an
     * actuator, its actor-table declaration -- survives until the next
     * reboot. It stops updating and disappears from this store right now;
     * its registry slot (and actor-table row, if any) is only reclaimed on
     * restart. Do not attempt to add registry deletion in this milestone. */

    /* An "outside" caller (webserver task), same as zigbee_permit_join()
     * above -- must hold the stack lock for this SDK call. */
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(ZB_LOCK_WAIT_MS))) {
        ESP_LOGW(TAG, "could not acquire the Zigbee stack lock; device removed from the "
                      "store but not asked to leave the network");
        return true;
    }
    esp_zb_zdo_mgmt_leave_req_param_t req = {
        .dst_nwk_addr = short_addr,
        .remove_children = 1,
    };
    memcpy(req.device_address, eui64, 8);
    esp_zb_zdo_device_leave_req(&req, NULL, NULL);
    esp_zb_lock_release();
    return true;
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

int zigbee_device_list(zb_device_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

bool zigbee_device_rename(const uint8_t eui64[8], const char *name)
{
    (void)eui64;
    (void)name;
    return false;
}

bool zigbee_device_remove(const uint8_t eui64[8])
{
    (void)eui64;
    return false;
}

#endif /* CONFIG_PLANTHUB_ZB_ENABLED */
