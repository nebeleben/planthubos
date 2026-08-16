#include "ble_collector.h"
#include "data_core.h"
#include "mibeacon.h"
#include "bthome.h"
#include "battery_sched.h"
#include "ble_collector_internal.h"
#include "adv_queue.h"
#include "swarm_store.h"
#include "rules.h"
#include "wrapper_index.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include <string.h>

static const char *TAG = "ble_collector";

#define XIAOMI_SVC_UUID 0xFE95
/* GAP scan units are 0.625 ms */
#define SCAN_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

/* M3 §1 raw-advert pipeline: gap_event (NimBLE host task) copies the raw
 * advertisement into this ring and returns; adv_decoder_task drains it and
 * does the actual decode (parsing, MiBeacon today, wrapper/BTHome dispatch
 * from Task 2 onward) plus every flash/registry access that decode implies.
 * The ring itself is a plain static -- 16 * 48 B budgeted (M3 spec §7),
 * sizeof(adv_ring_t) here is 712 B (16 * 44 B items + 8 B of head/tail/
 * full/dropped bookkeeping) -- rather than a heap-backed FreeRTOS queue
 * object, which would need its own separately allocated buffer for the same
 * data. s_adv_mux is a spinlock (portMUX_TYPE), not a SemaphoreHandle_t: the
 * producer (gap_event) must never block (see its own comment), and a
 * critical section around a single struct-copy push/pop is a few
 * instructions -- exactly the case portENTER_CRITICAL/portEXIT_CRITICAL
 * exist for, unlike s_batt_mutex below which guards a longer-lived table
 * across two independent tasks and can afford to take a real mutex. */
static adv_ring_t s_adv_ring;
static portMUX_TYPE s_adv_mux = portMUX_INITIALIZER_UNLOCKED;

/* M3 Task 2 (spec §2): the match index, built once at boot by
 * wrapper_store_load_all() (below, in ble_collector_start(), before the
 * decoder task or NimBLE scanning can start) and read-only from then on --
 * only Task 7's install/delete API will ever mutate it again (a later
 * task). 16 * 12 B = 192 B static, exactly the spec §7 budget line;
 * adv_decoder_task is the only reader, so this needs no lock of its own
 * (same reasoning as s_batt_tab needing one and this not: unlike s_batt_tab,
 * nothing outside the decoder task touches this table in M3 Task 2). */
static wrapper_index_t s_wrapper_index;

#define ADV_DECODER_TASK_STACK 3072
/* Below the NimBLE host task (configMAX_PRIORITIES - 4, see
 * nimble_port_freertos_init()/esp-idf's nimble_port_freertos.c) by a wide
 * margin -- this task must never preempt radio servicing, only mop up
 * behind it. Same low-priority band as this file's own battery poller
 * (battery_poll.c, prio 3) and pairing's background tasks. */
#define ADV_DECODER_TASK_PRIO  3
/* How long adv_decoder_task sleeps when the ring is empty before checking
 * again. MiFlora/BTHome/wrapper-target advertisement intervals are all
 * >= 100 ms in practice, so this adds no perceptible latency while keeping
 * the task off the CPU between advertisements. */
#define ADV_DECODER_POLL_MS    20

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

/* M3 Task 3 (spec §4): BTHome v2 is a built-in decoder, matched by its
 * service UUID exactly like a wrapper's WMATCH_SERVICE key would be, but
 * dispatched here directly -- never through s_wrapper_index -- because it
 * lives outside the 16-wrapper store entirely and costs none of a user's
 * wrapper slots. Runs on adv_decoder_task, same as the rest of
 * decode_adv_item() below (flash/NVS reads -- bindkey_get() -- are only
 * safe off the NimBLE host task).
 *
 * gap_mac is the raw GAP address exactly as gap_event captured it
 * (ble_addr_t.val[], on-air wire order) -- NOT yet the human/display order
 * bthome_decode()'s mac[] contract requires (see bthome.h's doc comment on
 * why: NimBLE's val[0] is the first octet transmitted, the byte order's
 * OWN least-significant end, confirmed against nimble/ble.h's
 * BLE_ADDR_IS_RPA family keying off val[5] for the address-type bits).
 * Reversed once here into `mac`, which is then used consistently for both
 * the AES-CCM nonce (inside bthome_decode()) and this device's identity
 * (device_id_from_mac()) -- the same order data_core.c already uses
 * everywhere else (via mibeacon.c's own reversed m.mac), so a BTHome
 * device's dashboard/bind-key identity matches what a user would read off
 * the device or a scanner app. */
static void decode_bthome_item(const uint8_t *data, size_t len, const uint8_t gap_mac[6])
{
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = gap_mac[5 - i];

    device_id_t id = device_id_from_mac(DEV_KIND_BLE, mac);
    char dev_id[24];
    device_id_format(&id, dev_id, sizeof dev_id);

    uint8_t key_buf[16];
    bool have_key = bindkey_get(dev_id, key_buf);

    bthome_emit_t emits[BTHOME_MAX_EMITS];
    size_t n = 0;
    bthome_err_t err = bthome_decode(data, len, have_key ? key_buf : NULL, mac, emits, &n);
    if (err != BTHOME_OK) {
        /* Same low-noise convention as the MiFlora path just below (a
         * mibeacon_parse() failure there is likewise never logged above
         * DEBUG): a malformed/undecryptable frame from a real BTHome
         * device is expected background noise (a wrong bind key, a frame
         * clipped by the queue's 31-byte payload cap, ...), not something
         * that should spam the log on every advertisement interval. */
        ESP_LOGD(TAG, "bthome: %s decode failed (err=%d)", dev_id, (int)err);
        return;
    }

    /* Each emit goes through data_core_submit_cap()'s own
     * capability_encode() + skip-on-CAP_VALUE_NONE discipline (M3 Task 3
     * brief: "the caller must SKIP the write in that case, never store the
     * sentinel") -- this function never calls capability_encode() itself. */
    bool wrote_any = false;
    for (size_t i = 0; i < n; i++) {
        if (data_core_submit_cap(mac, emits[i].cap_id, emits[i].value)) wrote_any = true;
    }
    /* Wake the rules engine only once real registry state changed --
     * mirrors "fires after a successful registry update, as the MiFlora
     * path does" (data_core_submit_from() -> rules_notify_value_update()
     * below), not the MiFlora path's own "every accepted frame regardless
     * of whether a value changed" looser trigger, since a BTHome frame with
     * every object out-of-range or unmapped (n==0, or every
     * data_core_submit_cap() skipped) wrote nothing for the engine to react
     * to. */
    if (wrote_any) rules_notify_value_update();
}

/* Runs on adv_decoder_task, never on the NimBLE host task -- this is where
 * flash reads / VM execution (Task 2 onward) are allowed to happen. For M3
 * Task 1 this is exactly the MiFlora decode that used to live inline in
 * gap_event, re-homed here unchanged (controller ruling on task-1-brief.md
 * Step 4: a build that stops reading its sensors is not an acceptable
 * intermediate state, even mid-milestone). Task 2 adds the match index and
 * dispatches to wrappers/BTHome on top of this. */
static void decode_adv_item(const adv_item_t *item)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, item->payload, item->len) != 0) return;

    /* M3 Task 2 matcher (spec §2): parse the advert exactly once (above) and
     * hand its service-UUID / manufacturer-id to the index, alongside the
     * item's own MAC for a mac_prefix match. wrapper_index_lookup()'s
     * contract is 0xFFFFFFFF for "this advert had none of that field" --
     * never 0, which is a real (if unlikely) UUID/company id. This runs for
     * every advert, MiFlora or not, since a non-MiFlora frame is exactly
     * what a wrapper exists to decode.
     *
     * Wrapper EXECUTION is not part of this task (Task 5 adds the arena and
     * the VM run) -- s_wrapper_index is only ever populated by
     * wrapper_store_load_all() at boot, and nothing installs a wrapper
     * before Task 7's API exists, so in practice this is always -1 today.
     * The lookup and its log line are wired now so the matcher itself is
     * exercised on real traffic ahead of Task 5 giving it something to do. */
    uint32_t svc_uuid = 0xFFFFFFFFu;
    if (fields.svc_data_uuid16 && fields.svc_data_uuid16_len >= 2) {
        svc_uuid = (uint32_t)(fields.svc_data_uuid16[0] | (fields.svc_data_uuid16[1] << 8));
    }
    uint32_t manu_id = 0xFFFFFFFFu;
    if (fields.mfg_data && fields.mfg_data_len >= 2) {
        manu_id = (uint32_t)(fields.mfg_data[0] | (fields.mfg_data[1] << 8));
    }

    /* M3 Task 3 (spec §4): BTHome dispatch happens BEFORE the wrapper index
     * is consulted -- it is a built-in decoder, held outside the
     * 16-wrapper store, never itself an entry in s_wrapper_index. A BTHome
     * advert still carries its 0xFCD2 service-data UUID like any other
     * service-keyed advert, so svc_uuid (computed just above) is reused
     * as-is rather than re-parsed. */
    if (svc_uuid == BTHOME_SVC_UUID) {
        decode_bthome_item(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, item->mac);
        return;
    }

    int wrapper_id = wrapper_index_lookup(&s_wrapper_index, svc_uuid, manu_id, item->mac);
    if (wrapper_id >= 0) {
        ESP_LOGD(TAG, "wrapper %d matched (execution lands in a later M3 task)", wrapper_id);
    }

    /* Native MiFlora path -- unchanged behaviour from before this task,
     * just reusing the single parse above instead of a second one. */
    if (!fields.svc_data_uuid16 || fields.svc_data_uuid16_len < 2 || svc_uuid != XIAOMI_SVC_UUID) return;
    mibeacon_t m;
    if (mibeacon_parse(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, &m) != MIBEACON_OK) return;
    if (m.product_id != MIBEACON_PRODUCT_MIFLORA) return;

    /* Direct reception: no relaying node, just heard (age_s = 0). item->rssi
     * already has NimBLE's 127 "RSSI unavailable" sentinel (see ble_gap.h)
     * mapped to 0 by gap_event before this was queued -- a raw int8_t 127
     * would otherwise read as an implausibly strong signal (+127 dBm)
     * rather than "unknown". Since M5b, this rssi feeds
     * registry_update_from()'s "strongest rssi wins" attribution
     * (best_rssi), so passing 127 through unfiltered would let a reading
     * with genuinely no signal information ever recorded outrank every real
     * measurement, including a legitimate node relay. 0 is the same
     * "unknown" value data_core_submit()'s wrapper already uses for callers
     * with no RSSI at all. */
    data_core_submit_from(&m, NULL, item->rssi, 0);
    /* Wake the rules engine (spec section 4 "Triggers"): a plain
     * event-group bit set, safe to call from any task -- same "short,
     * bounded, never blocks" standard data_core_submit_from() itself is
     * held to just above. data_core_submit_from() is void (no
     * merged/duplicate/full signal to gate on), so this fires for every
     * accepted MiFlora frame, not just ones that changed a value; the
     * engine's own 2s debounce (rules_engine.c) absorbs that, and
     * re-resolving to the same reading on a duplicate value is harmless,
     * not a spurious fire (rules_fsm.h's edge/level semantics key off the
     * condition result, not off "did a value change"). Safe before
     * rules_init() has run (rules.h). */
    rules_notify_value_update();

    /* Direct reception (this file only ever handles the hub's own radio,
     * never a node relay): record it as a battery-poll candidate. item->mac
     * / item->addr_type are the GAP address captured verbatim in gap_event
     * -- ble_addr_t's byte order does not match m.mac's, so it must never
     * be reconstructed from the MiBeacon MAC. item->uptime_s is the
     * esp_timer uptime captured at the moment gap_event received this
     * advertisement, not at decode time, so a queue backlog can't skew
     * "last seen" into the future. s_batt_tab is shared with
     * battery_poll.c's poller task -- always locked. */
    xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
    batt_sched_seen(s_batt_tab, m.mac, item->addr_type, item->mac, item->uptime_s);
    xSemaphoreGive(s_batt_mutex);
}

static void adv_decoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        adv_item_t item;
        portENTER_CRITICAL(&s_adv_mux);
        bool got = adv_ring_pop(&s_adv_ring, &item);
        portEXIT_CRITICAL(&s_adv_mux);
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(ADV_DECODER_POLL_MS));
            continue;
        }
        decode_adv_item(&item);
    }
}

/* Number of advertisements dropped because the ring was full when gap_event
 * tried to push -- exposed at GET /api/v1/status as "adv_dropped" (M3
 * spec §1: "so a saturated queue is visible rather than silent"). A single
 * uint32_t load is atomic on this target; no need for the critical section
 * push/pop take. */
uint32_t ble_collector_adv_dropped(void)
{
    return adv_ring_dropped(&s_adv_ring);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* This callback runs on the NimBLE host task: it must never touch
         * flash, never block, and never run a VM (M3 spec §1). So it does
         * exactly one thing -- copy the raw advertisement into a queue item
         * -- and returns; adv_decoder_task (above) does all the actual
         * decoding off this task. */
        adv_item_t item;
        item.len = (event->disc.length_data > ADV_PAYLOAD_MAX)
                       ? ADV_PAYLOAD_MAX : (uint8_t)event->disc.length_data;
        memcpy(item.payload, event->disc.data, item.len);
        if (item.len < ADV_PAYLOAD_MAX) {
            memset(item.payload + item.len, 0, ADV_PAYLOAD_MAX - item.len);
        }
        /* Raw GAP address, NOT the MiBeacon's own MAC (that's only known
         * once payload is decoded, off this task) -- see decode_adv_item()'s
         * comment on why the two must never be conflated. */
        memcpy(item.mac, event->disc.addr.val, 6);
        item.addr_type = event->disc.addr.type;
        /* NimBLE uses 127 as its "RSSI unavailable" sentinel (ble_gap.h);
         * mapped to 0 here, before queueing, so every consumer of
         * adv_item_t.rssi already sees the same "unknown" value
         * data_core_submit()'s wrapper uses -- see decode_adv_item()'s
         * fuller comment on why this matters for best-rssi attribution. */
        item.rssi = (event->disc.rssi == 127) ? 0 : event->disc.rssi;
        item.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

        portENTER_CRITICAL(&s_adv_mux);
        adv_ring_push(&s_adv_ring, &item);   /* drop-and-count on full; never blocks */
        portEXIT_CRITICAL(&s_adv_mux);
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

    /* Same ordering rule as s_batt_mutex above: the ring and its decoder
     * task must exist before nimble_port_freertos_init() below starts the
     * host task, since gap_event (which pushes to s_adv_ring) can start
     * running the instant that task exists. adv_ring_init() zeroes the
     * 712 B static ring (16 * 44 B items + bookkeeping -- M3 spec §7's
     * "16 * 48" budget); adv_decoder_task's own 3072 B stack is the only
     * heap cost this file's M3 pipeline adds, everything else above is
     * static. */
    adv_ring_init(&s_adv_ring);
    /* Also before the decoder task/host task can run: wrapper_store_load_all()
     * (M3 Task 2) does the boot's only wrapper-related LittleFS reads,
     * leaving s_wrapper_index fully built before adv_decoder_task's first
     * decode_adv_item() call can ever consult it. Static 192 B (16 * 12 B,
     * spec §7); the LittleFS scan itself is transient stack/heap, freed
     * before this returns. */
    wrapper_store_load_all(&s_wrapper_index);
    log_heap("before adv_decoder_task");
    if (xTaskCreate(adv_decoder_task, "ble_adv_decoder", ADV_DECODER_TASK_STACK,
                     NULL, ADV_DECODER_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(adv_decoder_task) failed");
        return ESP_ERR_NO_MEM;
    }
    log_heap("after adv_decoder_task (decoder task stack)");

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
