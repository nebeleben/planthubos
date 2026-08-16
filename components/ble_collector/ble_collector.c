#include "ble_collector.h"
#include "data_core.h"
#include "mibeacon.h"
#include "battery_sched.h"
#include "ble_collector_internal.h"
#include "swarm_store.h"
#include "rules.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

static const char *TAG = "ble_collector";

#define XIAOMI_SVC_UUID 0xFE95
/* GAP scan units are 0.625 ms */
#define SCAN_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

/* Directly-heard sensors, candidates for the daily battery poll (see
 * battery_poll.c). Written from this file's gap_event (NimBLE host task)
 * and from battery_poll.c's poller task (a distinct FreeRTOS task) -- two
 * genuinely different tasks, so every access to s_batt_tab, on either side,
 * must hold s_batt_mutex. This mirrors data_core.c's s_mutex around
 * s_registry, which IS how that file's registry access across tasks is
 * actually made safe -- not, as an earlier version of this comment wrongly
 * claimed, something that comes for free just because gap_event happens to
 * run on the same host task data_core_submit() is often called from. */
static batt_entry_t s_batt_tab[BATT_MAX_SENSORS];
static SemaphoreHandle_t s_batt_mutex;

/* battery_poll_start() and ble_collector_resume_scan() -- see
 * ble_collector_internal.h for both declarations. */

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .passive = 1,
        .itvl = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS),
        .window = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS),
        .filter_duplicates = 0,   /* we want repeated frames; registry dedups */
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    }
}

void ble_collector_resume_scan(void)
{
    start_scan();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) return 0;
        if (!fields.svc_data_uuid16 || fields.svc_data_uuid16_len < 2) return 0;
        uint16_t uuid = (uint16_t)(fields.svc_data_uuid16[0] | (fields.svc_data_uuid16[1] << 8));
        if (uuid != XIAOMI_SVC_UUID) return 0;
        mibeacon_t m;
        if (mibeacon_parse(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, &m) == MIBEACON_OK) {
            if (m.product_id != MIBEACON_PRODUCT_MIFLORA) return 0;
            /* Direct reception: no relaying node, just heard (age_s = 0).
             * event->disc.rssi is the advertisement's RSSI in dBm, EXCEPT
             * NimBLE uses 127 as its "RSSI unavailable" sentinel (see
             * ble_gap.h) -- a raw int8_t value that would otherwise read as
             * an implausibly strong signal (+127 dBm) rather than "unknown".
             * Since M5b, this rssi feeds registry_update_from()'s
             * "strongest rssi wins" attribution (best_rssi), so passing
             * 127 through unfiltered would let a reading with genuinely no
             * signal information ever recorded outrank every real
             * measurement, including a legitimate node relay. Map it to 0
             * instead, the same "unknown" value data_core_submit()'s
             * wrapper already uses for callers with no RSSI at all. */
            int8_t rssi = (event->disc.rssi == 127) ? 0 : event->disc.rssi;
            data_core_submit_from(&m, NULL, rssi, 0);
            /* Wake the rules engine (spec section 4 "Triggers"): a plain
             * event-group bit set, safe from this callback despite it
             * running on the NimBLE host task rather than a dedicated task
             * -- same "short, bounded, never blocks" standard
             * data_core_submit_from() itself is held to just above.
             * data_core_submit_from() is void (no merged/duplicate/full
             * signal to gate on), so this fires for every accepted MiFlora
             * frame, not just ones that changed a value; the engine's own
             * 2s debounce (rules_engine.c) absorbs that, and re-resolving
             * to the same reading on a duplicate value is harmless, not a
             * spurious fire (rules_fsm.h's edge/level semantics key off the
             * condition result, not off "did a value change"). Safe before
             * rules_init() has run (rules.h). */
            rules_notify_value_update();

            /* Direct reception (this file only ever handles the hub's own
             * radio, never a node relay): record it as a battery-poll
             * candidate. Address captured verbatim from the GAP event --
             * ble_addr_t's byte order does not match m.mac's, so it must
             * never be reconstructed from the MiBeacon MAC. s_batt_tab is
             * shared with battery_poll.c's poller task -- always locked. */
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
            batt_sched_seen(s_batt_tab, m.mac, event->disc.addr.type, event->disc.addr.val, now_s);
            xSemaphoreGive(s_batt_mutex);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();   /* should not happen with BLE_HS_FOREVER, but be safe */
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE synced, starting passive scan (itvl=%dms window=%dms)",
             CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS, CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Same permanent boot-time heap trace main.c keeps (see its log_heap()),
 * one level further down. main.c's "after ble_collector_start" milestone
 * measures this whole function as one ~70 KB step; these two split it into
 * the parts that are actually tunable separately: nimble_port_init() brings
 * up the BLE CONTROLLER (BT_CTRL_BLE_MAX_ACT instances, scan duplicate
 * cache, adv-report flow-control buffers) plus the NimBLE HOST pools
 * (msys 1/2, transport ACL/event pools -- all CONFIG_BT_NIMBLE_* sized),
 * while nimble_port_freertos_init() adds only the host task's own stack
 * (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE). Round 4 cut the connection-side
 * pools and the controller instance count on the grounds that this product
 * is a passive OBSERVER with at most ONE outbound connection at a time
 * (battery_poll.c); these two lines are how the next boot log proves how
 * much that actually bought, and where the remainder still sits. */
static void log_heap(const char *milestone)
{
    ESP_LOGI(TAG, "heap @ %s: free=%u B largest_free_block(8BIT|INTERNAL)=%u B",
             milestone, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

esp_err_t ble_collector_start(void)
{
    log_heap("before nimble_port_init");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    log_heap("after nimble_port_init (controller + host pools)");

    /* Created before nimble_port_freertos_init() below starts the host
     * task -- gap_event (which locks this) must never be able to run
     * before the mutex exists. */
    s_batt_mutex = xSemaphoreCreateMutex();
    if (!s_batt_mutex) {
        ESP_LOGE(TAG, "battery table mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    log_heap("after nimble_port_freertos_init (host task stack)");

    /* Node role: battery polling connects out and would fight ESP-NOW
     * timing on the node's single radio. Scanning/relaying (above) still
     * runs on both roles; only the poll timer/task is hub-only. */
    if (swarm_store_role() != SWARM_ROLE_NODE) {
        battery_poll_start(s_batt_tab, s_batt_mutex);
    }
    return ESP_OK;
}
