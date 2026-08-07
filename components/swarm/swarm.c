/* Wires espnow_link + pairing + swarm_store to data_core: the hub ingests
 * node-forwarded readings through data_core_submit_from() (the same door
 * the local BLE collector uses, via data_core_submit()/data_core_submit_from()),
 * and a node forwards its own locally-heard readings by subscribing to the
 * existing PLANTHUB_DATA_EVENT. Neither data_core nor ble_collector need
 * any further changes for this to work.
 */
#include "swarm.h"
#include "swarm_store.h"
#include "swarm_frame.h"
#include "swarm_buf.h"
#include "batt_cycle.h"
#include "node_ota.h"
#include "node_ota_recv.h"
#include "espnow_link.h"
#include "pairing.h"
#include "data_core.h"
#include "registry.h"
#include "mibeacon.h"
#include "app_config.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "swarm";

/* M7 final-review fix (F8): batt_cycle.c's batt_reconcile() (this component,
 * but built without visibility into swarm_frame.h/swarm_store.h) hard-codes
 * bare 0/1/2 literals for both the command it returns and the modes it
 * compares, tied to SWARM_CHECKIN_CMD_* / SWARM_PM_* only by comment (see
 * batt_cycle.h's own doc comment on batt_reconcile()). This file #includes
 * both real enums (swarm_frame.h, swarm_store.h) alongside batt_cycle.h, so
 * it's the one place that can actually check the comment's claim at compile
 * time -- a future reordering of either enum would otherwise silently
 * desync from batt_cycle.c's literals with no compiler diagnostic anywhere.
 * batt_cycle.c's literals: command 0=NONE/1=SET_MODE/2=STAY_AWAKE
 * (batt_reconcile()'s three `result.command = N` assignments); mode
 * 0=ALWAYS_ON/1=BATTERY_15/2=BATTERY_60 (period_table[]'s three entries,
 * indexed by power_mode). */
_Static_assert(SWARM_CHECKIN_CMD_NONE == 0, "batt_cycle.c's command literal 0 means NONE");
_Static_assert(SWARM_CHECKIN_CMD_SET_MODE == 1, "batt_cycle.c's command literal 1 means SET_MODE");
_Static_assert(SWARM_CHECKIN_CMD_STAY_AWAKE == 2, "batt_cycle.c's command literal 2 means STAY_AWAKE");
_Static_assert(SWARM_PM_ALWAYS_ON == 0, "batt_cycle.c's period_table[0]/mode literal 0 means ALWAYS_ON");
_Static_assert(SWARM_PM_BATTERY_15 == 1, "batt_cycle.c's period_table[1]/mode literal 1 means BATTERY_15");
_Static_assert(SWARM_PM_BATTERY_60 == 2, "batt_cycle.c's period_table[2]/mode literal 2 means BATTERY_60");

/* ---------------- Hub side: ingestion + per-node RAM stats ---------------- */

typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t last_seen_s;
    uint32_t frames_rx;
    int8_t   rssi;
    /* M7: the node's own last-reported power mode (SWARM_PM_*, swarm_store.h),
     * from its most recent accepted CHECKIN -- see checkin_task() below.
     * reported_mode_valid is false until the first CHECKIN this boot (a node
     * that has never checked in reports nothing, same "unknown, not a
     * guess" reasoning as rssi/last_seen_s being null until first heard). */
    uint8_t  reported_mode;
    bool     reported_mode_valid;
} node_stat_t;

static node_stat_t       s_stats[SWARM_MAX_NODES];
static SemaphoreHandle_t s_stats_mutex;
static uint32_t          s_frames_rx_total;

/* Called from the ESP-NOW receive callback (WiFi driver task): a short,
 * bounded critical section only (array scan over at most SWARM_MAX_NODES
 * entries, no I/O), same reasoning as data_core_submit()'s registry lock
 * below -- safe to take with portMAX_DELAY here because nothing that holds
 * s_stats_mutex ever blocks while holding it. */
static void record_stat(const uint8_t src[6], int rssi)
{
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);

    int idx = -1, free_idx = -1;
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, src, 6) == 0) { idx = i; break; }
        if (!s_stats[i].in_use && free_idx < 0) free_idx = i;
    }
    if (idx < 0) idx = free_idx;   /* -1 if the (tiny) table is somehow full */
    if (idx >= 0) {
        if (!s_stats[idx].in_use) {
            s_stats[idx].in_use = true;
            memcpy(s_stats[idx].mac, src, 6);
            s_stats[idx].frames_rx = 0;
        }
        s_stats[idx].last_seen_s = now_s;
        s_stats[idx].frames_rx++;
        s_stats[idx].rssi = (int8_t)rssi;
    }
    s_frames_rx_total++;

    xSemaphoreGive(s_stats_mutex);
}

/* Called from checkin_task() (below), never from the ESP-NOW receive
 * callback -- unlike record_stat(), which the CHECKIN branch of hub_rx_cb
 * calls directly (same as READING) to refresh frames_rx/last_seen_s/rssi
 * before this frame is even queued. By the time checkin_task() runs, that
 * call has already created/refreshed this node's slot, so the lookup here
 * is expected to always find one; a miss (defensively handled as a silent
 * no-op) would only mean the slot table is somehow full (SWARM_MAX_NODES
 * exceeded), same corner case record_stat() itself already tolerates. */
static void record_checkin_mode(const uint8_t mac[6], uint8_t mode)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0) {
            s_stats[i].reported_mode = mode;
            s_stats[i].reported_mode_valid = true;
            break;
        }
    }
    xSemaphoreGive(s_stats_mutex);
}

/* Hub: node_ota.c's node_ota_start() (Task 4) consults this to decide
 * whether a push should park (NODE_OTA_ST_PENDING_WAKE) rather than stream
 * immediately -- a node that last reported a battery mode is presumed
 * asleep between checkins, so streaming to it right away would talk to
 * nobody. Returns false (mode_out untouched) when this node has never sent
 * an accepted CHECKIN this boot -- callers must treat that as "unknown",
 * not as "ALWAYS_ON confirmed", same reasoning as the JSON's
 * reported_mode_valid below. Safe to call from any task: same short,
 * bounded, allocation-free scan under s_stats_mutex as every other
 * accessor in this file. */
bool swarm_node_reported_mode(const uint8_t mac[6], uint8_t *mode_out)
{
    if (!mac || !mode_out || !s_stats_mutex) return false;
    bool found = false;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0 && s_stats[i].reported_mode_valid) {
            *mode_out = s_stats[i].reported_mode;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_stats_mutex);
    return found;
}

/* A node-forwarded reading enters through exactly the same door as a
 * locally heard one, so storage, history, SSE and integrations need no
 * changes.
 *
 * data_core_submit_from() safety: verified against
 * components/data_core/data_core.c. It takes s_mutex with portMAX_DELAY,
 * but the only things ever done under that lock (here and in
 * data_core_snapshot()) are registry_find()/registry_update_from()/memcpy
 * -- a short, bounded, allocation-free critical section with no further
 * blocking calls inside it -- so contention is bounded to microseconds, not
 * indefinite. The event it posts afterwards uses esp_event_post(..., 0),
 * i.e. it never blocks even if the event queue is full (the post is simply
 * dropped). So data_core_submit_from() is safe to call directly from the
 * ESP-NOW receive callback (WiFi driver task); no deferral to another task
 * is needed for this path. */
static void ingest_reading(const uint8_t src[6], const swarm_reading_t *r, int rssi)
{
    mibeacon_t m;
    memset(&m, 0, sizeof(m));
    memcpy(m.mac, r->mac, 6);
    m.product_id = MIBEACON_PRODUCT_MIFLORA;
    m.frame_cnt = r->frame_cnt;
    if (r->temp_dc != INT16_MIN)      { m.temp_dc = r->temp_dc; m.has_temp = true; }
    if (r->moisture_pct != 0xFF)      { m.moisture_pct = r->moisture_pct; m.has_moisture = true; }
    if (r->battery_pct != 0xFF)       { m.battery_pct = r->battery_pct; m.has_battery = true; }
    if (r->lux != 0xFFFFFFFFu)        { m.lux = r->lux; m.has_lux = true; }
    if (r->conductivity_us != 0xFFFF) { m.conductivity_us = r->conductivity_us; m.has_conductivity = true; }
    /* Attribute to the relaying node, using the node's own rssi/age_s from
     * the reading (not the ESP-NOW link rssi passed to this function,
     * which is the hub's signal to the node, not the node's signal to the
     * sensor). */
    data_core_submit_from(&m, src, r->rssi, r->age_s);
    record_stat(src, rssi);
}

/* True if mac is in swarm_store's persisted node table -- the same set
 * espnow_link_init() restores as ESP-NOW peers at boot. swarm_store's
 * table is a mutex-guarded RAM cache (see swarm_store.c), so this is a
 * short, bounded, allocation-free lookup over at most SWARM_MAX_NODES
 * entries: safe to call directly from the ESP-NOW receive callback, same
 * reasoning as every other swarm_store/s_stats_mutex access already made
 * from this path. No NVS/flash touched here. */
static bool is_paired_node(const uint8_t mac[6])
{
    int n = swarm_store_node_count();
    for (int i = 0; i < n; i++) {
        uint8_t stored[6];
        if (swarm_store_node_at(i, stored, NULL) && memcmp(stored, mac, 6) == 0) return true;
    }
    return false;
}

/* ---------------- Hub side: CHECKIN reconciliation (M7) ----------------
 *
 * hub_rx_cb (the ESP-NOW receive callback, WiFi driver task) must never
 * send, write NVS, or block -- same project-wide rule as every other
 * deferred-work path in this file (record_stat()'s own comment, pairing.c's
 * pong_task/forget_task). CHECKIN's ack additionally needs
 * batt_reconcile() (pure, cheap) and node_ota_notify_checkin() (RAM-only,
 * non-blocking) evaluated per item, plus an espnow_link_send() -- all of
 * that belongs on a dedicated task, not the callback, exactly like
 * pairing.c's pong_task is the reference for "callback queues, task
 * sends". */
typedef struct {
    uint8_t         mac[6];
    swarm_checkin_t checkin;
} checkin_item_t;

#define CHECKIN_QUEUE_LEN 4

static QueueHandle_t s_checkin_queue;
static TaskHandle_t  s_checkin_task;

static void checkin_task(void *arg)
{
    (void)arg;
    checkin_item_t item;
    for (;;) {
        if (xQueueReceive(s_checkin_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        /* hub_rx_cb already called record_stat() for this item before
         * queuing it (same as READING) -- this only adds the self-reported
         * mode on top, so GET /api/v1/nodes (Task 6) and node_ota_start()
         * (node_ota.c, via swarm_node_reported_mode()) both see it. */
        record_checkin_mode(item.mac, item.checkin.power_mode);

        uint8_t desired = (uint8_t)swarm_store_node_desired_mode(item.mac);
        /* M7 final-review fix (F1/F2): OR in node_ota_active_for() alongside
         * the original parked-only node_ota_pending_for() check, so
         * batt_reconcile()'s STAY_AWAKE-over-SET_MODE priority keeps applying
         * for this node's ENTIRE session, not only while it's parked waiting
         * to start. Parked-only missed two cases: (a) an always-on node's
         * own periodic checkin (always_on_checkin_task()) landing a SET_MODE
         * ack -- and the esp_restart() that follows -- while this hub is
         * mid-stream to it would abort the transfer; that node already
         * treats STAY_AWAKE as a no-op (see always_on_checkin_task()'s own
         * comment), so widening the condition here is enough to suppress the
         * reboot. (b) a battery node whose STAY_AWAKE ack (sent the moment
         * node_ota_notify_checkin() releases its park) is lost in the air:
         * node_ota_pending_for() alone would already read false by then (the
         * session left PENDING_WAKE the instant it was released), so its
         * next checkin could get NONE/SET_MODE instead and deep-sleep out
         * from under an in-flight stream. node_ota_active_for() stays true
         * through that whole window, so a retried checkin still gets
         * STAY_AWAKE. */
        bool ota_pending = node_ota_pending_for(item.mac) || node_ota_active_for(item.mac);
        batt_cmd_t cmd = batt_reconcile(desired, item.checkin.power_mode, ota_pending);

        if (cmd.command == SWARM_CHECKIN_CMD_STAY_AWAKE) {
            /* Releases a parked OTA session targeting this node, if one
             * exists (node_ota.c's NODE_OTA_ST_PENDING_WAKE) -- a no-op
             * otherwise. The STAY_AWAKE ack below is sent regardless: the
             * node keeps its radio on either way (Task 5), whether or not
             * there actually was a session waiting for it. */
            node_ota_notify_checkin(item.mac);
        }

        swarm_checkin_ack_t ack = {
            .version = SWARM_PROTO_VERSION,
            .type = SWARM_MSG_CHECKIN_ACK,
            .command = cmd.command,
            .arg = cmd.arg,
        };
        uint8_t buf[sizeof(ack)];
        size_t n = swarm_encode_checkin_ack(&ack, buf, sizeof(buf));
        if (n == 0) {
            ESP_LOGE(TAG, "CHECKIN_ACK for " MACSTR ": failed to encode", MAC2STR(item.mac));
            continue;
        }

        /* Unicast -- unlike PAIR_ACK/PONG/FORGET, a checked-in node is
         * already an adopted, encrypted ESP-NOW peer (is_paired_node()
         * gated this in hub_rx_cb before it was ever queued), so there is
         * no AP-association/no-peer-yet reason to broadcast here. A send
         * failure is logged and dropped, not retried: the node will check
         * in again next cycle (or is already awake waiting, per Task 5),
         * and reconciliation is naturally idempotent -- batt_reconcile()
         * recomputes the right command from scratch every time, so a
         * missed ack just delays convergence by one checkin, it never
         * leaves the node in a wrong state permanently. */
        esp_err_t err = espnow_link_send(item.mac, buf, n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CHECKIN_ACK -> " MACSTR " failed (%s), dropped -- reconciliation "
                          "self-heals next checkin", MAC2STR(item.mac), esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "CHECKIN_ACK -> " MACSTR ": command=%u arg=%u (desired=%u reported=%u ota_pending=%d)",
                     MAC2STR(item.mac), cmd.command, cmd.arg, desired, item.checkin.power_mode, ota_pending);
        }
    }
}

/* Idempotent; safe to call more than once. Must run before any CHECKIN can
 * be answered -- swarm_start_main() calls this right after espnow_link_init()
 * so the responder exists from hub boot onward, same eager-init reasoning as
 * pairing_hub_init()'s own doc comment (a battery node may check in at any
 * time, independent of any operator action). */
static esp_err_t ensure_checkin_task(void)
{
    if (s_checkin_task) return ESP_OK;
    if (!s_checkin_queue) s_checkin_queue = xQueueCreate(CHECKIN_QUEUE_LEN, sizeof(checkin_item_t));
    if (!s_checkin_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(checkin_task, "swarm_checkin", 3072, NULL, 3, &s_checkin_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ensure_checkin_task: xTaskCreate failed");
        s_checkin_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void hub_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    int type = swarm_frame_type(data, (size_t)len);
    if (type == SWARM_MSG_READING) {
        /* PlanV1 3.3 promises "unpaired frames are dropped, so a neighbour
         * cannot inject readings" -- ESP-NOW hands this callback ANY
         * unencrypted frame from ANY MAC in range, paired or not, so that
         * promise has to be enforced here explicitly rather than assumed.
         * Without this check, any device broadcasting a well-formed
         * SWARM_MSG_READING could inject arbitrary (sensor MAC, values)
         * straight into data_core -> registry -> SSE/sampler/history,
         * no pairing window or claim key required. PAIR_REQ is
         * deliberately NOT gated this way -- that path is only ever live
         * during an operator-opened window and is pairing_handle_frame's
         * job to police. */
        if (!is_paired_node(src_mac)) {
            /* WARN (not DEBUG) and includes every stored node MAC being
             * compared against, not just the rejected sender -- this gate
             * started rejecting a legitimate, hardware-confirmed-paired
             * node immediately after 47f3db9 put the node's radio into
             * APSTA mode, and the leading theory is that ESP-NOW frames
             * from a node now egress tagged with its SoftAP interface MAC
             * (conventionally base-MAC + 1) rather than the STA MAC that
             * got stored at pairing time -- a mismatch that was invisible
             * before this gate existed, since nothing previously compared
             * the two. This log is diagnostic evidence for that, not a
             * fix: still rate-limited to once per 5s so a noisy/hostile
             * neighbour can't flood the console, and the gate's behaviour
             * is unchanged -- do not weaken or remove it without first
             * confirming what src_mac actually is here. */
            static int64_t s_last_drop_log_us;
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_drop_log_us > 5000000) {
                char known[SWARM_MAX_NODES * 20 + 8] = "";
                int n = swarm_store_node_count();
                for (int i = 0; i < n; i++) {
                    uint8_t mac[6];
                    if (!swarm_store_node_at(i, mac, NULL)) continue;
                    char one[20];
                    snprintf(one, sizeof(one), "%s" MACSTR, (known[0] != '\0') ? ", " : "", MAC2STR(mac));
                    strlcat(known, one, sizeof(known));
                }
                ESP_LOGW(TAG, "dropping READING from " MACSTR ", known nodes: %s",
                         MAC2STR(src_mac), n > 0 ? known : "(none)");
                s_last_drop_log_us = now_us;
            }
            return;
        }
        swarm_reading_t r;
        if (swarm_decode_reading(data, (size_t)len, &r)) ingest_reading(src_mac, &r, rssi);
        return;
    }
    if (type == SWARM_MSG_OTA_STATUS) {
        /* Node -> BROADCAST, plaintext (fix, M5c hardware round 1 -- see the
         * doc comment on swarm_ota_status_t in swarm_frame.h for why; do not
         * re-derive it back to unicast/encrypted). Unlike the pre-fix
         * unicast/encrypted shape, successfully receiving this frame at all
         * says nothing about the sender's identity any more -- anyone in
         * radio range can broadcast a well-formed OTA_STATUS. is_paired_node()
         * below is therefore now the PRIMARY gate, not defense-in-depth: it
         * is what stops a non-node from injecting one. node_ota_handle_status()
         * is the second, session-specific gate -- it independently re-checks
         * src against whichever node the CURRENT session actually targets
         * AND requires the frame's session_id to match the hub's own
         * esp_random() value for that session (swarm_frame.h), so even a
         * spoofed source MAC from a real paired node cannot be credited to a
         * session it wasn't part of. node_ota_handle_status() only ever
         * records/enqueues -- see its own header comment -- so it is exactly
         * as safe to call from this callback as is_paired_node()/record_stat()
         * already are. */
        if (!is_paired_node(src_mac)) return;
        swarm_ota_status_t st;
        if (swarm_decode_ota_status(data, (size_t)len, &st)) node_ota_handle_status(src_mac, &st);
        return;
    }
    if (type == SWARM_MSG_CHECKIN) {
        /* Node -> hub, unicast, encrypted (M7): same pairing/spoofing
         * reasoning as READING above -- ESP-NOW hands this callback any
         * frame from any MAC in range, so is_paired_node() is the gate that
         * keeps an unpaired device from injecting a CHECKIN and steering
         * this hub's reconciliation logic. record_stat() is called here,
         * synchronously, same as READING -- by the time checkin_task()
         * (below) picks this item off the queue, this node's stats slot is
         * guaranteed to already exist for record_checkin_mode() to update.
         * Everything past that (batt_reconcile(), node_ota_notify_checkin(),
         * the espnow_link_send() ack) is deferred to checkin_task(): this
         * callback only ever queues, never sends -- see ensure_checkin_task()'s
         * doc comment and pairing.c's pong_task, the reference for this
         * exact pattern. A full/missing queue just drops this one CHECKIN;
         * the node retries on its own schedule (Task 5), so nothing is lost
         * permanently. */
        if (!is_paired_node(src_mac)) return;
        swarm_checkin_t c;
        if (!swarm_decode_checkin(data, (size_t)len, &c)) return;
        record_stat(src_mac, rssi);

        checkin_item_t item;
        memcpy(item.mac, src_mac, 6);
        item.checkin = c;
        if (!s_checkin_queue || xQueueSend(s_checkin_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "CHECKIN from " MACSTR ": no checkin task available or queue full, dropping",
                     MAC2STR(src_mac));
        }
        return;
    }
    /* PAIR_REQ/PAIR_ACK, PING/PONG, FORGET or anything unrecognised:
     * pairing_handle_frame already filters to the types it understands and
     * silently ignores everything else, so handing it anything that isn't a
     * reading or an OTA status is safe and keeps this dispatcher small. */
    pairing_handle_frame(src_mac, data, len, rssi);
}

/* Logs the hub's effective regulatory domain once it actually associates.
 * espnow_link_init() already logs a country snapshot at boot, but the hub
 * now runs 802.11d/AUTO policy (M5c, PlanV1 3.3/8f): at that early boot
 * point wifi_manager_start() has only just been asked to connect, so
 * esp_wifi_get_country() there can only ever report the compile-time
 * CONFIG_PLANTHUB_WIFI_COUNTRY default, not whatever the router's beacons
 * actually advertise. This handler fires once real association happens
 * (IP_EVENT_STA_GOT_IP already implies WIFI_EVENT_STA_CONNECTED preceded
 * it), by which point 802.11d has had a real beacon to learn from, so its
 * read-back is the domain PAIR_ACK will actually hand to a newly-adopted
 * node -- making a country mismatch (or a router still on the "01"
 * world-safe default) visible at a glance instead of silently inferred. */
static void log_effective_country(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    wifi_country_t country;
    if (esp_wifi_get_country(&country) == ESP_OK) {
        ESP_LOGI(TAG, "effective wifi country after association: %c%c%c, usable channels %u-%u",
                 country.cc[0], country.cc[1], country.cc[2], country.schan,
                 country.schan + country.nchan - 1);
    } else {
        ESP_LOGW(TAG, "esp_wifi_get_country failed after association; cannot confirm effective country");
    }
}

esp_err_t swarm_start_main(void)
{
    if (!s_stats_mutex) s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) return ESP_ERR_NO_MEM;

    esp_err_t err = espnow_link_init(hub_rx_cb);
    if (err != ESP_OK) return err;

    /* Hub only (this function never runs for a node): logs the router's
     * actually-adopted country the moment association happens, and again
     * on every reconnect (a router could change its own advertised domain,
     * e.g. after a firmware update) -- see log_effective_country() above.
     * Registration failure is logged, not fatal: the hub still works, only
     * this one diagnostic line would be missing. */
    esp_err_t ev_err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, log_effective_country, NULL);
    if (ev_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to register post-association country log handler: %s", esp_err_to_name(ev_err));
    }

    /* Brings up the SWARM_MSG_PONG responder unconditionally, not only
     * once an operator opens a pairing window -- a node may call
     * pairing_node_resync_channel() (real liveness check, protocol v2) at
     * any time, independent of pairing state, so the responder must
     * already exist by then. Failure here is logged but not fatal to
     * bringing the hub up: forwarding/ingest still work without it, only
     * node-side resync liveness confirmation would silently never succeed. */
    err = pairing_hub_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pairing_hub_init failed: %s -- PING liveness probes will go unanswered",
                 esp_err_to_name(err));
    }

    /* Same eager-init reasoning as pairing_hub_init() just above: a battery
     * node (M7) may check in at any time, independent of any operator
     * action, so the responder must already exist. Failure is logged but
     * not fatal to bringing the hub up -- see ensure_checkin_task()'s own
     * comment; without it, CHECKIN frames are simply dropped at the queue
     * send in hub_rx_cb, same graceful-degradation shape as every other
     * "responder unavailable" case in this file. */
    err = ensure_checkin_task();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ensure_checkin_task failed: %s -- CHECKIN frames will go unanswered",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "swarm (main) started on channel %u", espnow_link_channel());
    return ESP_OK;
}

/* M7: power_mode's wire string. Shared naming (not shared code) with
 * api_v1.c's POST .../{MAC12} parser, which maps the same three strings
 * back to swarm_power_mode_t -- kept as two small, independent tables
 * rather than a cross-component header, since the enum is tiny/stable and
 * not worth the extra coupling. */
static const char *power_mode_str(swarm_power_mode_t m)
{
    switch (m) {
    case SWARM_PM_BATTERY_15: return "battery_15";
    case SWARM_PM_BATTERY_60: return "battery_60";
    case SWARM_PM_ALWAYS_ON:
    default:                  return "always_on";
    }
}

int swarm_node_list_json(char *buf, size_t cap)
{
    node_stat_t snap[SWARM_MAX_NODES];
    uint32_t total;
    /* Snapshotted once, up front, so every node in this response is aged
     * against the same instant rather than drifting across however long
     * cJSON construction below takes -- irrelevant in practice at this
     * scale, but "one now_s per response" is the honest way to compute an
     * age at all. */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        memcpy(snap, s_stats, sizeof(snap));
        total = s_frames_rx_total;
        xSemaphoreGive(s_stats_mutex);
    } else {
        memset(snap, 0, sizeof(snap));
        total = 0;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "nodes");

    /* The definitive node list is swarm_store's PERSISTENT node table --
     * every adopted node lives there and survives a reboot. s_stats above
     * is only ever populated when a frame is actually received, so an
     * adopted node that hasn't transmitted (or hasn't been heard) yet
     * since this boot would previously be missing here entirely, even
     * though GET /api/v1/status already reported "paired":true for it --
     * a contradiction that made a one-sided-pairing defect look like a
     * hub-side bug when it wasn't. Merge in s_stats where available and
     * report null for last_seen_s/rssi (a real, meaningful "never heard"
     * value) and 0 for frames_rx (a real, meaningful count) otherwise.
     *
     * last_seen_s is an AGE in seconds (now_s - the record_stat() timestamp),
     * not the raw stored timestamp (fix, M5c hardware round 4, defect 3):
     * record_stat() stores esp_timer_get_time()/1e6 -- the hub's own uptime
     * at the moment it last heard this node -- and emitting that verbatim
     * made a client rendering "last seen N seconds ago" simply wrong (a
     * round 4 API poll showed last_seen_s climbing 802 -> 882 while
     * frames_rx was actively incrementing 98 -> 111, i.e. moving in exactly
     * the wrong direction for an age while the node was demonstrably alive).
     * Converting to an age here means every consumer of this JSON gets a
     * value that means what its name says, without having to separately
     * fetch GET /api/v1/status's uptime_s and subtract it themselves the
     * way the webui's Nodes tab used to. */
    int n_nodes = swarm_store_node_count();
    for (int i = 0; i < n_nodes; i++) {
        uint8_t mac[6];
        if (!swarm_store_node_at(i, mac, NULL)) continue;

        const node_stat_t *stat = NULL;
        for (int j = 0; j < SWARM_MAX_NODES; j++) {
            if (snap[j].in_use && memcmp(snap[j].mac, mac, 6) == 0) { stat = &snap[j]; break; }
        }

        char macstr[18];
        snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", macstr);
        char name[SWARM_NODE_NAME_LEN + 1];
        if (swarm_store_node_name(mac, name) && name[0] != '\0') cJSON_AddStringToObject(o, "name", name);
        else cJSON_AddNullToObject(o, "name");
        if (stat) {
            /* now_s and last_seen_s are both esp_timer_get_time()/1e6 off the
             * same monotonic clock, so now_s >= last_seen_s always holds in
             * practice; the clamp is just defensive floor-at-zero, same
             * spirit as the next_offset clamp in node_ota.c. */
            uint32_t age_s = (now_s >= stat->last_seen_s) ? (now_s - stat->last_seen_s) : 0;
            cJSON_AddNumberToObject(o, "last_seen_s", age_s);
        } else {
            cJSON_AddNullToObject(o, "last_seen_s");
        }
        cJSON_AddNumberToObject(o, "frames_rx", stat ? stat->frames_rx : 0);
        if (stat) cJSON_AddNumberToObject(o, "rssi", stat->rssi);
        else cJSON_AddNullToObject(o, "rssi");
        /* M7: the node's own last-reported power mode (SWARM_PM_*), from its
         * most recent accepted CHECKIN this boot -- see checkin_task()/
         * record_checkin_mode() above. reported_mode_valid is false (and
         * reported_mode null) until that first CHECKIN, same "unknown, not
         * a guess" reasoning as last_seen_s/rssi being null before this node
         * has ever been heard from at all -- a node can be a known/paired
         * ALWAYS_ON device that simply hasn't checked in yet (it never needs
         * to, if it never sleeps), so reported_mode_valid=false must not be
         * read as "reported ALWAYS_ON". */
        bool mode_valid = stat && stat->reported_mode_valid;
        cJSON_AddBoolToObject(o, "reported_mode_valid", mode_valid);
        if (mode_valid) cJSON_AddNumberToObject(o, "reported_mode", stat->reported_mode);
        else cJSON_AddNullToObject(o, "reported_mode");
        /* M7: "power_mode" is the node's DESIRED mode (swarm_store's
         * per-node table, set by POST /api/v1/nodes/{MAC12}
         * {"power_mode":...}) -- deliberately NOT reported_mode above. This
         * is the field the UI's power-mode control edits, so it must show
         * operator intent immediately, even before any CHECKIN confirms the
         * node actually picked it up; reported_mode/reported_mode_valid
         * above remain the only source of truth for what the node itself
         * last said. "power_mode_pending" is how the UI knows a desired
         * change hasn't been confirmed by the node yet: true when the most
         * recent CHECKIN's mode differs from desired, OR when this node
         * hasn't checked in at all this boot and desired isn't ALWAYS_ON --
         * a fresh non-ALWAYS_ON desire always needs at least one more
         * checkin to take effect. ALWAYS_ON is excluded from that second
         * case because it is the node's own power-on default: an
         * ALWAYS_ON-desired node that has simply never needed to check in
         * yet is not "pending" anything. */
        swarm_power_mode_t desired = swarm_store_node_desired_mode(mac);
        cJSON_AddStringToObject(o, "power_mode", power_mode_str(desired));
        bool pending = mode_valid ? (stat->reported_mode != (uint8_t)desired)
                                   : (desired != SWARM_PM_ALWAYS_ON);
        cJSON_AddBoolToObject(o, "power_mode_pending", pending);
        /* "buffered": Task 5's RAM ring (swarm.c's forward_task) tracks a
         * NODE's own undelivered-reading backlog, but that state lives only
         * on the node itself -- there is no wire message carrying a
         * backlog depth back to the hub (would need its own protocol
         * extension, out of scope for M5b). This is therefore honestly
         * null, not a best-effort guess, for every entry until such a
         * message exists. */
        cJSON_AddNullToObject(o, "buffered");
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "frames_rx_total", total);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;
    int n = snprintf(buf, cap, "%s", body);
    free(body);
    return (n >= 0 && (size_t)n < cap) ? n : -1;
}

uint32_t swarm_frames_rx(void)
{
    if (!s_stats_mutex) return 0;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    uint32_t n = s_frames_rx_total;
    xSemaphoreGive(s_stats_mutex);
    return n;
}

/* Called from the forget HTTP handler's task (api_v1.c) only -- NOT from
 * hub_rx_cb/record_stat's path, so this adds nothing new that's reachable
 * from the ESP-NOW receive callback. Same short, bounded, allocation-free
 * scan over at most SWARM_MAX_NODES entries as record_stat(), under the
 * same mutex. */
void swarm_forget_node_stats(const uint8_t mac[6])
{
    if (!s_stats_mutex) return;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0) {
            memset(&s_stats[i], 0, sizeof(s_stats[i]));
            break;
        }
    }
    xSemaphoreGive(s_stats_mutex);
}

#define SWARM_FORGET_BROADCAST_COUNT 3
#define SWARM_FORGET_BROADCAST_GAP_MS 200

/* One-shot task: broadcasts SWARM_MSG_FORGET (now carrying the target's
 * MAC -- see swarm_frame.h) a few times, then deletes itself. Deliberately
 * a plain FreeRTOS task, spawned fresh per forget -- NOT the ESP-NOW
 * receive callback (which must never send) and not the httpd request task
 * either (a blocking ~600ms sleep there would stall the HTTP response to
 * the operator's browser for no reason). Broadcast, not unicast: by the
 * time this runs, api_v1.c's forget handler has already called
 * espnow_link_remove_peer() for the target, so there is no peer left to
 * address a unicast frame to at all (mirrors PAIR_ACK/PONG's broadcast
 * reasoning in swarm_frame.h, just for the opposite reason -- those
 * broadcast because the peer doesn't exist YET, this because it no longer
 * does). Best-effort and fire-and-forget: nothing waits on the result,
 * matching the plan's "a node that was powered off still needs the BOOT
 * button" acceptance -- there is no ack for FORGET to wait for.
 *
 * Fixed, M5c: swarm_forget_t now carries the forgotten node's MAC
 * (target_mac). ESP-NOW's own broadcast address is still untargeted -- every
 * node paired to this hub still RECEIVES this frame -- but each one now
 * checks target_mac against its own STA MAC (pairing.c's FORGET handling)
 * before checking sender identity, so only the actual target ever acts on
 * it; every other paired node now correctly ignores it instead of also
 * unpairing. `arg` is a heap-allocated 6-byte MAC, owned by this task and
 * freed here -- xTaskCreate's caller (swarm_broadcast_forget() below) may
 * return and its own stack copy of the MAC may go away before this task
 * ever actually runs, so the MAC has to travel via the heap, not a stack
 * pointer. */
static void forget_broadcast_task(void *arg)
{
    uint8_t target_mac[6];
    memcpy(target_mac, arg, 6);
    free(arg);

    swarm_forget_t f = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_FORGET };
    memcpy(f.target_mac, target_mac, 6);
    uint8_t buf[sizeof(f)];
    size_t n = swarm_encode_forget(&f, buf, sizeof(buf));

    for (int i = 0; i < SWARM_FORGET_BROADCAST_COUNT; i++) {
        if (n) {
            esp_err_t err = espnow_link_broadcast(buf, n);
            ESP_LOGI(TAG, "FORGET(" MACSTR ") broadcast %d/%d: %s",
                     MAC2STR(target_mac), i + 1, SWARM_FORGET_BROADCAST_COUNT, esp_err_to_name(err));
        }
        if (i + 1 < SWARM_FORGET_BROADCAST_COUNT) vTaskDelay(pdMS_TO_TICKS(SWARM_FORGET_BROADCAST_GAP_MS));
    }
    vTaskDelete(NULL);
}

/* Hub: called by api_v1.c's DELETE /api/v1/nodes/{MAC12} handler after it
 * has already removed `mac` from swarm_store and its ESP-NOW peer entry --
 * this only kicks off the best-effort radio notification, it does not
 * touch swarm_store or the peer table itself. Safe to call from the httpd
 * task: this function itself never blocks, it only spawns the task above
 * (after copying mac onto the heap for that task to own). */
void swarm_broadcast_forget(const uint8_t mac[6])
{
    uint8_t *arg = malloc(6);
    if (!arg) {
        ESP_LOGE(TAG, "swarm_broadcast_forget: out of memory -- forgotten node will not learn it "
                      "over the air; BOOT-button recovery is still available");
        return;
    }
    memcpy(arg, mac, 6);
    if (xTaskCreate(forget_broadcast_task, "swarm_forget_bc", 3072, arg, 5, NULL) != pdPASS) {
        free(arg);
        ESP_LOGE(TAG, "swarm_broadcast_forget: failed to create broadcast task -- "
                      "forgotten node(s) will not learn it over the air; "
                      "BOOT-button recovery is still available");
    }
}

/* ---------------- Node side: forwarding ---------------- */

#define SWARM_FWD_QUEUE_LEN     8
#define SWARM_FWD_FAIL_THRESHOLD 5

static QueueHandle_t s_fwd_queue;
static uint8_t       s_hub_mac[6];   /* set once in swarm_start_node(); MAC never
                                       * changes across a resync, only the channel does */

/* ---------------- Node side: CHECKIN_ACK hand-off (M7 Task 5) ----------------
 *
 * node_rx_cb (the ESP-NOW receive callback, WiFi driver task) must never
 * block or touch NVS -- same project-wide rule as every other deferred-work
 * path in this file. A CHECKIN_ACK's consumer (swarm_node_battery_cycle(),
 * on its own dedicated task, and the always-on periodic checkin below, on
 * ITS own task) does real work with it (persist mode/counters, esp_restart(),
 * esp_deep_sleep()), so the callback's only job is decode + a cheap RAM-only
 * source check against the stored hub MAC + a non-blocking send onto this
 * depth-1 queue. Depth 1 is enough: this node has at most one CHECKIN
 * outstanding at a time (whichever of the two checkin paths above is
 * currently active for this boot's power mode), and both consumers drain
 * any stale entry before sending a fresh CHECKIN, so an ack left over from a
 * previous, already-timed-out wait can never be misread as the answer to a
 * later one. Created eagerly, in swarm_start_node() below, before
 * espnow_link_init() hands node_rx_cb its first frame -- same eager-init
 * reasoning as s_health_queue above (a CHECKIN_ACK could arrive as soon as
 * the receive callback is live, including for the always-on periodic
 * checkin, which needs this queue even when the battery-cycle task is never
 * created at all). */
static QueueHandle_t s_checkin_ack_queue;

/* ---------------- Node side: OTA rollback-guard health signal (M5c) ----------------
 *
 * ota_post.h's ota_rollback_guard_node_confirm() performs a flash write
 * (otadata), so it must never be called from the ESP-NOW receive callback
 * (node_rx_cb, below -- the WiFi driver task). Same DEFERRAL pattern as
 * pairing.c's forget_task/pong_task: the callback only enqueues (a
 * non-blocking send, depth-1 queue -- once one confirmation is queued
 * there is nothing further to add, the guard confirms at most once), a
 * dedicated task does the actual call.
 *
 * UNLIKE pairing.c's ensure_forget_task()/ensure_hub_task(), the queue and
 * task here are created EAGERLY, in swarm_start_node() below, not lazily on
 * first use: signal_node_healthy() is called from TWO different tasks --
 * node_rx_cb (the WiFi driver task, on an accepted PONG) and forward_task
 * (on its own task, on a successful send) -- so a lazy "if (!s_health_task)
 * create it" check could race between them (both observe NULL, both create
 * a task/queue, one handle gets silently overwritten and leaked). Every
 * lazy-init precedent elsewhere in this codebase (pairing.c's
 * ensure_forget_task/ensure_hub_task) is only ever called from ONE task (a
 * receive callback processes frames strictly one at a time, so it cannot
 * race itself), which does not apply here -- hence eager init instead of
 * copying that pattern. */
static void (*s_health_cb)(const char *reason);
static QueueHandle_t s_health_queue;

static void health_confirm_task(void *arg)
{
    (void)arg;
    char reason[24];
    for (;;) {
        if (xQueueReceive(s_health_queue, reason, portMAX_DELAY) != pdTRUE) continue;
        if (s_health_cb) s_health_cb(reason);
    }
}

void swarm_node_set_health_cb(void (*cb)(const char *reason))
{
    s_health_cb = cb;
}

/* Called once from swarm_start_node(), before anything that could call
 * signal_node_healthy() below is live (espnow_link_init()/forward_task()
 * both start after this). */
static esp_err_t ensure_health_task(void)
{
    if (s_health_queue) return ESP_OK;
    s_health_queue = xQueueCreate(1, sizeof(char[24]));
    if (!s_health_queue) return ESP_ERR_NO_MEM;
    TaskHandle_t task;
    BaseType_t ok = xTaskCreate(health_confirm_task, "swarm_health", 2560, NULL, 5, &task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ensure_health_task: xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Safe to call from any task, including the ESP-NOW receive callback
 * (node_rx_cb): only ever does a non-blocking queue send here, never the
 * flash write itself. A dropped signal (queue momentarily full, or
 * ensure_health_task() never having been called -- e.g. a hub, which never
 * calls it at all) just means confirmation is delayed to the next call, or
 * simply never happens on a role that never needed it -- forward_task()
 * calls this on every successful send, and node_rx_cb calls it on every
 * accepted PONG, so there are repeated chances on a node, not just one. */
static void signal_node_healthy(const char *reason)
{
    if (!s_health_queue) return;
    char buf[24];
    strlcpy(buf, reason, sizeof(buf));
    xQueueSend(s_health_queue, buf, 0);
}

/* M7 Task 5 fix (code review): a SYNCHRONOUS variant of signal_node_healthy(),
 * used only by the two SET_MODE reboot sites below (always_on_checkin_task()
 * and swarm_node_battery_cycle()). Those need the OTA rollback-guard
 * confirmation to have actually happened BEFORE esp_restart() reboots the
 * device -- signal_node_healthy()'s normal path only queues a request for
 * health_confirm_task to pick up whenever it's next scheduled, which could
 * easily lose the race against an esp_restart() called right after it.
 * Still routed through the same function-pointer indirection (s_health_cb)
 * as signal_node_healthy(), never a direct call into ota_post.h -- see
 * swarm_node_set_health_cb()'s own doc comment in swarm.h for why a direct
 * dependency on webserver/ota_post.h from this component would be circular
 * (webserver already depends on swarm via api_v1.c). ota_post.h documents
 * ota_rollback_guard_node_confirm() -- what s_health_cb is wired to, in
 * main.c -- as safe to call from any task and idempotent past the first
 * successful confirm, so calling it directly here, on this dedicated task,
 * rather than via the queue, is safe. A no-op (s_health_cb is NULL) on a
 * hub, which never registers a callback. */
static void confirm_health_before_restart(const char *reason)
{
    if (s_health_cb) s_health_cb(reason);
}

static void node_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    int type = swarm_frame_type(data, (size_t)len);

    /* OTA_BEGIN/OTA_CHUNK/OTA_ABORT (M5c Task 5): a node's receiver
     * (node_ota_recv.c) validates the sender against the stored hub MAC
     * itself before doing anything else, same defense-in-depth pattern as
     * every other type handled directly in a receive callback in this
     * codebase -- so no additional check is needed here. Decode failure
     * (malformed/wrong-length frame) is silently dropped, same as every
     * other decoder call on this path. */
    if (type == SWARM_MSG_OTA_BEGIN) {
        swarm_ota_begin_t begin;
        if (swarm_decode_ota_begin(data, (size_t)len, &begin)) node_ota_recv_handle_begin(src_mac, &begin);
        return;
    }
    if (type == SWARM_MSG_OTA_CHUNK) {
        swarm_ota_chunk_t chunk;
        if (swarm_decode_ota_chunk(data, (size_t)len, &chunk)) node_ota_recv_handle_chunk(src_mac, &chunk);
        return;
    }
    if (type == SWARM_MSG_OTA_ABORT) {
        swarm_ota_abort_t ab;
        if (swarm_decode_ota_abort(data, (size_t)len, &ab)) node_ota_recv_handle_abort(src_mac, &ab);
        return;
    }

    if (type == SWARM_MSG_CHECKIN_ACK) {
        /* M7 Task 5: decode, source-check against the stored hub MAC (same
         * short, bounded, allocation-free RAM-cache read already used for
         * this exact check elsewhere on this path -- see the PONG branch
         * just below), then a non-blocking send onto the depth-1 queue
         * above. Nothing else past that happens here -- see that queue's
         * own comment for why the actual command handling is deferred.
         *
         * Node-side OTA rollback-guard health signal (spec §4): "the
         * existing node health signal (first delivered reading, or a
         * received ack) confirms it" -- spec §4's own wording extends the
         * M5c health criteria (forward_task()'s first delivered reading, or
         * a PONG, both wired below/elsewhere) to a received CHECKIN_ACK
         * too, same reasoning as the PONG branch just below: this is proof
         * the hub's application layer just processed a frame from this
         * node. This closes a real gap for a battery node -- unlike an
         * always-on node, a battery wake never sends PING (no resync unless
         * a checkin actually fails), so without this, a node with nothing
         * local to forward (no sensors currently in range) would have NO
         * way to ever confirm an OTA'd image via the criteria M5c
         * originally shipped, and swarm_node_battery_cycle()'s
         * rollback-sleep retry loop (below) would spin forever on an
         * always-succeeding-but-never-confirmed checkin. */
        swarm_checkin_ack_t ack;
        if (swarm_decode_checkin_ack(data, (size_t)len, &ack)) {
            uint8_t hub_mac[6];
            if (swarm_store_hub(hub_mac, NULL, NULL) && memcmp(src_mac, hub_mac, 6) == 0) {
                signal_node_healthy("CHECKIN_ACK received");
                if (s_checkin_ack_queue) xQueueSend(s_checkin_ack_queue, &ack, 0);
            }
        }
        return;
    }

    if (type == SWARM_MSG_PONG) {
        /* Node-side OTA rollback-guard health signal (M5c): receiving a
         * PONG from this node's own stored hub is the plan's explicit
         * alternative criterion to "delivered a reading" -- proof the
         * hub's application layer just processed a frame from this node,
         * the same liveness bar pairing_node_resync_channel() itself uses.
         * Deliberately looser than pairing.c's own PONG handling just
         * below (no nonce match, no s_resync_waiting gate): any genuine
         * PONG from the real hub is good enough evidence of connectivity
         * for this purpose, not just one that happens to answer a resync
         * currently in flight. swarm_store_hub() is the same short,
         * bounded, allocation-free RAM-cache read already used for this
         * exact check elsewhere on this receive-callback path (is_paired_node(),
         * pairing.c's own PONG/FORGET handling) -- safe here too. Falls
         * through to pairing_handle_frame() below regardless (that call
         * still owns the actual resync-match logic). */
        swarm_pong_t pong;
        if (swarm_decode_pong(data, (size_t)len, &pong)) {
            uint8_t hub_mac[6];
            if (swarm_store_hub(hub_mac, NULL, NULL) && memcmp(src_mac, hub_mac, 6) == 0) {
                signal_node_healthy("PONG received");
            }
        }
    }

    /* A node only ever expects PAIR_ACK/PONG/FORGET here (initial pairing,
     * resync liveness, or a forget notification); pairing_handle_frame()
     * ignores anything else. Readings are never received here, only sent. */
    pairing_handle_frame(src_mac, data, len, rssi);
}

/* data_core already posts this only when registry_update() reported NEW data
 * (a changed MiBeacon frame counter), so the node inherits the same dedup the
 * hub uses and transmits ~100x less than one frame per advertisement. Runs
 * on the default event-loop task: must not block, so it only builds the
 * frame and hands it to forward_task() via a queue -- the actual radio send
 * happens on that dedicated task instead. */
static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const uint8_t *mac = data;
    static registry_t snap;      /* event-loop task only */
    data_core_snapshot(&snap);
    int idx = registry_find(&snap, mac);
    if (idx < 0) return;
    const sensor_entry_t *se = &snap.sensors[idx];
    const mibeacon_t *m = &se->latest;

    swarm_reading_t r = {
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_READING,
        .frame_cnt = m->frame_cnt,
        .temp_dc = m->has_temp ? m->temp_dc : INT16_MIN,
        .moisture_pct = m->has_moisture ? m->moisture_pct : 0xFF,
        .battery_pct = m->has_battery ? m->battery_pct : 0xFF,
        .lux = m->has_lux ? m->lux : 0xFFFFFFFFu,
        .conductivity_us = m->has_conductivity ? m->conductivity_us : 0xFFFF,
        /* This node's own BLE signal to the sensor -- Task 1 (M5b) added
         * best_rssi to sensor_entry_t precisely so this is available here.
         * On a node, data_core_submit_from() is only ever called locally
         * with via_node == NULL (ble_collector hears the sensor directly,
         * same as on a hub), so best_rssi IS this node's own reading of
         * it, never another node's -- there is no node-to-node relaying in
         * M5b. This is the hub's "strongest RSSI wins" attribution input
         * (registry_update_from()); reporting a hardcoded 0 here (as M5a
         * did, before best_rssi existed) made every node's contribution
         * look identically weak and left that attribution inert. */
        .rssi = se->best_rssi,
        .age_s = 0,  /* just heard */
        ._pad = 0,
    };
    memcpy(r.mac, mac, 6);

    if (!s_fwd_queue || xQueueSend(s_fwd_queue, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "forward queue full, dropping reading for " MACSTR, MAC2STR(mac));
    }
}

/* ---------------- Node side: RAM-only backlog for undelivered readings ----------------
 *
 * A node that can't currently reach the hub (hub rebooting, brief outage)
 * would otherwise simply lose whatever it was sending right then -- this
 * ring rides out exactly that, for a bounded amount of history. It is
 * DELIBERATELY RAM-only and lost on node reboot: it exists to survive a
 * *hub* outage, not to be a durable store of its own, and every reading in
 * it is already sitting in the live registry too (data_core_snapshot() can
 * always rebuild "current" state), so a node reboot loses only the backlog
 * of already-superseded history, never the current reading. Owned
 * exclusively by forward_task() below -- nothing else ever reads or writes
 * it, so it needs no lock (the ring's own mechanics carry no lock either --
 * see swarm_buf.h -- for the same reason: a single owner needs none). The
 * push/pop/evict/FIFO/age-clamp mechanics themselves live in swarm_buf.c/.h,
 * a pure-C unit extracted specifically so tests/host/test_swarm_buf.c can
 * exercise them without any FreeRTOS/ESP-IDF dependency -- this was M5b's
 * one untested piece of logic. */
static swarm_buf_t s_buf;

/* Buffers a reading that just failed to send. When full, the oldest entry is
 * evicted to make room -- logged at debug with a running counter, per the
 * brief, rather than silently discarding without any trace. */
static void buffer_push(const swarm_reading_t *r, int64_t now_us)
{
    bool was_full = swarm_buf_count(&s_buf) == SWARM_NODE_BUFFER_LEN;
    if (was_full) {
        ESP_LOGD(TAG, "reading buffer full (%d), dropping oldest for " MACSTR
                      " (dropped=%" PRIu32 " total, about to become %" PRIu32 ")",
                 SWARM_NODE_BUFFER_LEN, MAC2STR(s_buf.entries[s_buf.head].r.mac),
                 swarm_buf_dropped(&s_buf), swarm_buf_dropped(&s_buf) + 1);
    }
    swarm_buf_push(&s_buf, r, now_us);
}

/* Owns every espnow_link_send() the node makes for readings, so a slow or
 * unreachable radio never stalls the default event-loop task (same
 * reasoning as sse.c's httpd_queue_work). On repeated failures, triggers a
 * channel resync rather than silently dropping forever.
 *
 * Buffering: a reading that fails to send (live or a backlog retry) goes
 * into the RAM ring above rather than being dropped outright. A live
 * reading is always preferred over the backlog when both are available --
 * checked fresh at the top of every loop iteration, non-blocking -- so a
 * long backlog (up to SWARM_NODE_BUFFER_LEN entries deep after an outage)
 * never delays current data: at most one buffered reading is sent per live
 * reading interval, draining gradually rather than in one blocking burst.
 * This all runs on this dedicated task, never on the ESP-NOW receive
 * callback (WiFi driver task), per the project-wide rule. */
static void forward_task(void *arg)
{
    (void)arg;
    swarm_reading_t r;
    int consec_fail = 0;
    /* One-shot: a working node->hub link is otherwise only inferable from
     * the hub side (frames_rx climbing in GET /api/v1/nodes) -- this makes
     * it positively visible on the node's own console the first time it
     * actually happens. */
    bool first_delivered = false;

    for (;;) {
        bool have_reading = xQueueReceive(s_fwd_queue, &r, 0) == pdTRUE;
        bool from_backlog = false;

        if (!have_reading) {
            swarm_buf_entry_t br;
            if (swarm_buf_pop(&s_buf, &br)) {
                r = br.r;
                /* age_s is recomputed here, at transmit time, not at the
                 * moment it was (re)buffered -- the whole point of
                 * buffering is riding out an outage of unknown length, so
                 * the hub must see how stale this reading actually is right
                 * now. r.age_s already carries whatever age had accumulated
                 * before this buffering, so swarm_buf_recompute_age() ADDS
                 * the additional wait on top of it, compounding correctly
                 * across repeated buffer/retry cycles (and clamping at
                 * UINT16_MAX rather than wrapping -- data_core's own
                 * DATA_CORE_MAX_AGE_S (30 min) will drop it hub-side long
                 * before that matters anyway). */
                r.age_s = swarm_buf_recompute_age(r.age_s, br.captured_us, esp_timer_get_time());
                have_reading = true;
                from_backlog = true;
            } else {
                /* Nothing live, nothing buffered: block until a live
                 * reading arrives. Draining is only ever driven by this
                 * task noticing the buffer is non-empty, never a timer, so
                 * there is nothing else useful to do while both are empty. */
                if (xQueueReceive(s_fwd_queue, &r, portMAX_DELAY) != pdTRUE) continue;
                have_reading = true;
            }
        }
        if (!have_reading) continue;

        uint8_t buf[sizeof(r)];
        size_t n = swarm_encode_reading(&r, buf, sizeof(buf));
        if (n == 0) continue;

        esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
        if (err == ESP_OK) {
            consec_fail = 0;
            if (!first_delivered) {
                first_delivered = true;
                ESP_LOGI(TAG, "first reading delivered to hub");
                /* Node-side OTA rollback-guard health signal (M5c): the
                 * plan's primary criterion, "successfully delivered a
                 * reading to its hub". Only needs signalling once -- see
                 * signal_node_healthy()/ota_rollback_guard_node_confirm(),
                 * both idempotent past their first call -- so this rides
                 * the same first_delivered latch as the log line above
                 * rather than firing on every single successful send. */
                signal_node_healthy("reading delivered to hub");
            }
            continue;
        }

        consec_fail++;
        ESP_LOGW(TAG, "reading send failed (%s), consecutive=%d", esp_err_to_name(err), consec_fail);
        /* Buffer whatever just failed -- live or a backlog entry that failed
         * again on retry -- rather than dropping it. */
        buffer_push(&r, esp_timer_get_time());
        if (consec_fail >= SWARM_FWD_FAIL_THRESHOLD) {
            esp_err_t rerr = pairing_node_resync_channel();
            ESP_LOGI(TAG, "resync after %d consecutive failures: %s",
                     consec_fail, esp_err_to_name(rerr));
            consec_fail = 0;
        }

        /* Backoff, but only while draining the backlog: a LIVE reading that
         * fails still falls straight through to the top of the loop (a
         * fresh live reading may already be waiting, and preferring it over
         * a stale backlog is the whole point of the check at the top of
         * this loop), same as before this change. The backlog case is
         * different. M5a had no backlog at all, so once a node's small
         * live traffic dried up during a hub outage this task simply ran
         * out of anything to send and blocked on the queue receive further
         * up (portMAX_DELAY) -- that blocking self-limited the retry rate
         * for free. The backlog broke that: once it's non-empty this task
         * never blocks any more -- pop, send (espnow_link.c's send_blocking()
         * resolves a failure within its own ~200ms completion wait, not the
         * multi-second span of a full channel sweep), re-buffer, loop -- so
         * a prolonged hub outage now has the node spending essentially all
         * of its time either send-failing or, every SWARM_FWD_FAIL_THRESHOLD
         * failures, sweeping all 13 channels (~6.5s) for a hub that isn't
         * there. A one-second delay here caps that to roughly one attempt
         * per second, which is plenty fast to notice the hub coming back
         * while not spinning for the length of the outage. This matters
         * more than the CPU cost alone suggests:
         * M7's battery-powered nodes will pay for every one of these
         * attempts in radio-on wake time, so an unthrottled retry loop
         * during a multi-hour hub outage would be a real, avoidable battery
         * cost, not just wasted cycles. */
        if (from_backlog) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* Brings up WiFi far enough for ESP-NOW without ever joining any network:
 * no esp_wifi_connect() call, ever -- this device is simply never
 * associated to any AP, which is what keeps espnow_link_set_channel()
 * free to hop channels for pairing/resync. No STA/AP netif either (ESP-NOW
 * operates directly on the WiFi MAC layer and needs no IP netif -- the
 * upstream esp-now example brings WiFi up the same way, and this held up
 * fine on real hardware for plain STA mode; nothing here requires a netif
 * for APSTA either since the softAP configured below is a decoy with no
 * IP-level function).
 *
 * WIFI_MODE_APSTA, not plain STA: confirmed on real hardware that a
 * unicast ESP-NOW frame from an AP-associated hub (the main hub is a
 * normal WiFi STA) gets silently filtered -- and never MAC-acked -- by an
 * unassociated node's radio. That's the well-known ESP-NOW + WiFi
 * mixed-mode coexistence trap, and Espressif's documented workaround for a
 * device that must exchange ESP-NOW frames with an AP-associated peer is
 * exactly this: bring the receiving side up in APSTA. (The handshake
 * itself was additionally fixed to use broadcast for PAIR_ACK, which
 * sidesteps this for that one frame regardless -- see pairing.c's
 * hub_task() -- but APSTA is also what should make the steady-state
 * node->hub unicast DATA frames reliable after pairing.) The softAP is a
 * throwaway: hidden (ssid_hidden) and configured for zero stations
 * (max_connection = 0), so it neither clutters the air nor accepts
 * anyone. Do NOT apply any of this to the hub -- its STA association must
 * keep working exactly as it does today; this function is node-only.
 *
 * WIFI_STORAGE_RAM + an explicit empty STA config matter for a reason that
 * only showed up on real hardware: wifi_manager's start_sta() (used when
 * this same device was previously a hub) calls esp_wifi_set_config() with
 * the driver's default WIFI_STORAGE_FLASH, which makes the WiFi driver
 * itself -- independently of app_config's own separate copy of the
 * credentials -- persist that STA config into its own flash-backed NVS
 * blob and auto-reload it on every esp_wifi_init(). Left alone, that meant
 * esp_wifi_start() here picked the old SSID/password back up and kept
 * trying to associate ("Haven't to connect to a suitable AP now!" every
 * ~300ms), fighting the pairing sweep's channel hops the whole time.
 * Switching to RAM storage and clearing the driver's live STA config (a
 * volatile, this-boot-only change) stops it -- app_config's own stored
 * credentials are never touched here, so they still work if this device
 * is later switched back to a main hub. */

/* ---------------- Node side: CHECKIN send/wait, shared by both checkin
 * paths below (M7 Task 5) ---------------- */

/* Builds and sends one CHECKIN frame reporting `mode`/`wake_counter`, then
 * waits up to BATT_CHECKIN_WAIT_MS for the CHECKIN_ACK node_rx_cb queues in
 * response (see s_checkin_ack_queue's own comment above). Drains any stale
 * entry first -- a previous wait that already gave up (returned false
 * below, or a resync round below it) may have left one behind, and that
 * must never be mistaken for the answer to THIS send. Returns true (and
 * fills *ack_out) only for a genuine ack received after this call's own
 * send; false on a send failure or a timed-out wait. Blocking
 * (espnow_link_send() and the queue wait both block), so this must only
 * ever run on a dedicated task, never node_rx_cb -- both callers below
 * satisfy that (swarm_node_battery_cycle()'s own task, and
 * always_on_checkin_task() below). */
static bool send_checkin_and_wait_ack(uint8_t mode, uint32_t wake_counter, swarm_checkin_ack_t *ack_out)
{
    if (s_checkin_ack_queue) {
        swarm_checkin_ack_t stale;
        while (xQueueReceive(s_checkin_ack_queue, &stale, 0) == pdTRUE) { /* drain leftovers */ }
    }

    swarm_checkin_t c = {
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_CHECKIN,
        .power_mode = mode,
        .wake_counter = wake_counter,
    };
    uint8_t buf[sizeof(c)];
    size_t n = swarm_encode_checkin(&c, buf, sizeof(buf));
    if (n == 0) {
        ESP_LOGE(TAG, "CHECKIN: failed to encode");
        return false;
    }

    esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CHECKIN send failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!s_checkin_ack_queue) return false;
    return xQueueReceive(s_checkin_ack_queue, ack_out, pdMS_TO_TICKS(BATT_CHECKIN_WAIT_MS)) == pdTRUE;
}

/* One checkin round (spec §4 step 3 / failure-honesty paragraph): send +
 * wait, and on no ack, one bounded resync attempt -- the existing
 * single-sweep channel resync, same call forward_task() itself falls back
 * to after repeated send failures -- followed by exactly one more send +
 * wait. A battery node must never burn its wake budget sweeping repeatedly
 * (spec §4's own "failure honesty" wording); still no ack after that one
 * retry means this checkin round counts failed. */
static bool do_checkin_round(uint8_t mode, uint32_t wake_counter, swarm_checkin_ack_t *ack_out)
{
    if (send_checkin_and_wait_ack(mode, wake_counter, ack_out)) return true;

    ESP_LOGW(TAG, "CHECKIN: no ack, attempting one bounded resync before counting this round failed");
    esp_err_t rerr = pairing_node_resync_channel();
    ESP_LOGI(TAG, "battery-cycle resync: %s", esp_err_to_name(rerr));

    return send_checkin_and_wait_ack(mode, wake_counter, ack_out);
}

/* M7 Task 5 fix (code review): shared by both SET_MODE reboot sites below
 * and the rollback-sleep gate further down -- a single place for the
 * esp_ota_get_running_partition()/esp_ota_get_state_partition() pair
 * instead of three copies of the same two calls. */
static bool running_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    return running != NULL
        && esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY;
}

/* ---------------- Node side: always-on periodic checkin (M7 Task 5, spec §4/§6) ----------------
 *
 * A battery node's own checkin cycle (swarm_node_battery_cycle(), below)
 * only exists while power_mode != ALWAYS_ON. Without an equivalent for an
 * ALWAYS_ON node, a hub-side desired-mode change (an operator switching
 * this node to a battery mode from the webui) would have no way to ever
 * reach it -- the "makes mode changes deliverable" requirement this task
 * exists to close (Global Constraints). Runs unconditionally from
 * swarm_start_node() below, for every paired node regardless of mode, and
 * re-checks the CURRENT power mode on every tick rather than gating once at
 * task-create time -- that also covers the one case a node's mode can
 * change WITHOUT a reboot: swarm_node_battery_cycle()'s own failed-wake
 * ALWAYS_ON fallback (below) sets the mode and simply returns, no
 * esp_restart(). From that moment this task is what keeps mode changes
 * deliverable again, exactly as if the node had been ALWAYS_ON from boot. */
static void always_on_checkin_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BATT_ALWAYS_ON_CHECKIN_S * 1000));

        if (swarm_store_power_mode() != SWARM_PM_ALWAYS_ON) {
            /* A battery-mode node's own cycle owns checkins for as long as
             * it stays in that mode -- a second, competing CHECKIN from
             * here too would just double up on the hub's reconciliation
             * for no benefit, and race send_checkin_and_wait_ack()'s
             * single-outstanding-checkin assumption (the depth-1 queue). */
            continue;
        }

        swarm_checkin_ack_t ack;
        if (!send_checkin_and_wait_ack(SWARM_PM_ALWAYS_ON, swarm_store_wake_counter(), &ack)) continue;

        if (ack.command == SWARM_CHECKIN_CMD_SET_MODE) {
            esp_err_t serr = swarm_store_set_power_mode((swarm_power_mode_t)ack.arg);
            if (serr != ESP_OK) {
                /* Code review fix (issue 6): only reboot once the new mode
                 * is actually persisted -- rebooting on an unpersisted
                 * write would come back up still ALWAYS_ON, having thrown
                 * away the reboot for nothing. Not persisted is not lost:
                 * the hub keeps re-sending SET_MODE on every future checkin
                 * until reported/desired agree (batt_reconcile() is
                 * idempotent, per checkin_task()'s own comment), so simply
                 * not rebooting this round is enough -- just loop back
                 * around to the next periodic checkin. */
                ESP_LOGE(TAG, "always-on CHECKIN_ACK: SET_MODE %u -- failed to persist (%s), not "
                              "rebooting; the hub will re-send this command", ack.arg, esp_err_to_name(serr));
                continue;
            }
            /* Code review fix (issue 3): a deliberate SET_MODE reboot rolls
             * back a good PENDING_VERIFY image exactly like an unconfirmed
             * sleep-triggered reset would (the bootloader reverts any
             * PENDING_VERIFY image that was never explicitly confirmed
             * before the next boot) -- so confirm synchronously, right
             * here, before rebooting, mirroring the rollback-sleep rule's
             * own reasoning in swarm_node_battery_cycle() below. */
            if (running_image_pending_verify()) {
                ESP_LOGW(TAG, "always-on CHECKIN_ACK: SET_MODE %u -- running image is PENDING_VERIFY, "
                              "confirming before rebooting", ack.arg);
                confirm_health_before_restart("mode change");
            }
            ESP_LOGW(TAG, "always-on CHECKIN_ACK: SET_MODE %u -- persisted, rebooting into the new mode",
                     ack.arg);
            esp_restart();
        }
        /* NONE or STAY_AWAKE: an always-on node is already awake with
         * nothing to skip sleeping for, so STAY_AWAKE needs no special
         * handling here -- batt_reconcile() only ever emits it for a node
         * whose REPORTED mode is a battery one with a pending OTA (hub.c's
         * checkin_task()/node_ota.c's node_ota_start()), which this branch,
         * reporting ALWAYS_ON, never triggers. */
    }
}

/* ---------------- Node side: battery-mode wake cycle (M7 Task 5, spec §4) ---------------- */

/* Code review fix (issue 1): pace + bound for the PENDING_VERIFY retry loop
 * in swarm_node_battery_cycle() below -- see that loop's own comment for
 * the full "unpaced CHECKIN storm" rationale. 5s between passes, 24 passes
 * (~2 minutes total) before giving up and forcing the ALWAYS_ON fallback. */
#define BATT_PENDING_VERIFY_RETRY_DELAY_MS 5000u
#define BATT_PENDING_VERIFY_MAX_RETRIES    24u

/* Code review fix (issue 2): a session can legitimately take a while to
 * even START after the STAY_AWAKE ack -- the hub's own hash pass over the
 * image is ~1-1.5s, and node_ota_recv.c's handle_begin() calls
 * esp_ota_begin() before it ever sets active=true, which erases the target
 * partition first (~3-5s on this hardware) -- together that can eat most of
 * a naive 10-consecutive-idle-second budget before node_ota_recv_active()
 * ever reports true even once, making a slow-erase part deep-sleep mid-
 * handshake deterministically, not as a rare edge case. Splitting the wait
 * into two budgets fixes that: a longer grace period during which "not
 * active yet" is expected and not counted at all, THEN the original
 * consecutive-idle rule, but only once a session has actually been
 * observed active at least once (so it is now answering "did it end?", a
 * question the pre-fix code was also asking too early). */
#define BATT_STAY_AWAKE_NO_SESSION_GRACE_S 60u  /* no session observed active yet -- generous
                                                  * next to hub-hash (~1-1.5s) + node erase
                                                  * (~3-5s) so a slow-erase part is not
                                                  * mistaken for "never coming" */
#define BATT_STAY_AWAKE_IDLE_LIMIT         10u  /* consecutive idle 1s polls, AFTER a session
                                                  * has been observed active, before assuming
                                                  * it ended without a reboot */

/* Blocks up to BATT_STAY_AWAKE_CAP_S while node_ota_recv reports an active
 * session (spec §6's node-side stay-awake cap: "the OTA session's 10-minute
 * total timeout plus the checkin-to-start gap"). STAY_AWAKE's ack means the
 * hub just released a session it had parked pending this node's wake (see
 * checkin_task()'s node_ota_notify_checkin() call on the hub side). Two
 * phases, per the review fix above: before any session has been observed
 * active, "not active" just means "not started yet" and is tolerated for up
 * to BATT_STAY_AWAKE_NO_SESSION_GRACE_S; once one HAS been observed active,
 * BATT_STAY_AWAKE_IDLE_LIMIT consecutive idle polls means it ended without a
 * reboot -- failed/aborted, per the brief -- so this gives up and resumes
 * the normal cycle. A genuine SUCCESS instead reboots the node from inside
 * node_ota_recv.c itself (finalize_session()'s esp_restart()), which ends
 * this whole task along with everything else, so that outcome is never
 * observed here directly -- there is nothing left to "resume" in that
 * case. */
static void battery_stay_awake_wait(void)
{
    uint32_t elapsed_s = 0;
    uint32_t idle_consecutive = 0;
    bool session_seen = false;
    while (elapsed_s < BATT_STAY_AWAKE_CAP_S) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_s++;
        if (node_ota_recv_active()) {
            session_seen = true;
            idle_consecutive = 0;
            continue;
        }
        if (!session_seen) {
            if (elapsed_s >= BATT_STAY_AWAKE_NO_SESSION_GRACE_S) {
                ESP_LOGW(TAG, "STAY_AWAKE: no OTA session observed active within %us of the ack, "
                              "giving up and resuming the wake cycle", (unsigned)BATT_STAY_AWAKE_NO_SESSION_GRACE_S);
                return;
            }
            continue;
        }
        idle_consecutive++;
        if (idle_consecutive >= BATT_STAY_AWAKE_IDLE_LIMIT) {
            ESP_LOGI(TAG, "STAY_AWAKE: no active OTA session for %" PRIu32 "s, resuming the wake cycle",
                     idle_consecutive);
            return;
        }
    }
    ESP_LOGW(TAG, "STAY_AWAKE: cap of %us reached without the OTA session completing, resuming the wake cycle",
             (unsigned)BATT_STAY_AWAKE_CAP_S);
}

/* Code review fix (issue 1 pacing bound / issue 4): shared by both
 * fallback sites in swarm_node_battery_cycle() below -- the original
 * failed-wake-limit fallback, and the new PENDING_VERIFY retry-cap
 * fallback the pacing fix below adds. Persists ALWAYS_ON AND resets the
 * failed-wake counter to 0 together: leaving the counter at whatever
 * value triggered the fallback (e.g. BATT_FAILED_WAKE_LIMIT) would mean a
 * later hub-issued SET_MODE back into a battery mode re-trips this same
 * fallback after just ONE subsequent failed wake, instead of getting a
 * fresh run at the full limit the way a newly-configured battery node
 * would -- the counter is documented (spec §4) to reset "on any
 * successful checkin", and re-entering a battery mode fresh is exactly
 * that case. */
static void battery_enter_always_on_fallback(const char *reason)
{
    ESP_LOGW(TAG, "battery cycle: falling back to ALWAYS_ON (%s) so this node stays reachable", reason);
    esp_err_t perr = swarm_store_set_power_mode(SWARM_PM_ALWAYS_ON);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "battery cycle: failed to persist the ALWAYS_ON fallback (%s); this node may "
                      "stay stuck in its battery cycle", esp_err_to_name(perr));
    }
    esp_err_t ferr = swarm_store_set_failed_wakes(0);
    if (ferr != ESP_OK) {
        ESP_LOGW(TAG, "battery cycle: failed to reset the failed-wake counter after fallback (%s)",
                 esp_err_to_name(ferr));
    }
}

esp_err_t swarm_node_battery_cycle(void)
{
    int64_t wake_start_us = esp_timer_get_time();

    uint8_t mode = (uint8_t)swarm_store_power_mode();
    if (mode == SWARM_PM_ALWAYS_ON) {
        /* Defensive: main.c's caller already gates on this before creating
         * the task this function runs on, but batt_sleep_us()'s documented
         * precondition ("callers must not invoke this for power_mode =
         * ALWAYS_ON") makes re-checking here, rather than trusting the
         * caller blindly, worth the one extra branch. */
        return ESP_OK;
    }

    /* Step 1 (spec §4): increment + persist the NVS wake counter. Once per
     * wake, here at the top -- NOT inside the retry loop below, which only
     * repeats the checkin/ack/bookkeeping steps, never the wake itself. */
    uint32_t wake_counter = swarm_store_wake_counter() + 1;
    esp_err_t cerr = swarm_store_set_wake_counter(wake_counter);
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "battery cycle: failed to persist wake counter (%s), continuing anyway",
                 esp_err_to_name(cerr));
    }

    /* Step 2 (spec §4): scan window. Readings collect and forward through
     * the already-running scan -> ring -> forward_task() machinery
     * (started by swarm_start_node(), a precondition of this function)
     * while this task simply waits -- no separate scan logic needed here. */
    vTaskDelay(pdMS_TO_TICKS(BATT_SCAN_WINDOW_S * 1000));

    /* Code review fix (issue 1): bounds the PENDING_VERIFY retry loop
     * further down. Counts only passes that actually hit that branch (not
     * every loop iteration), paced BATT_PENDING_VERIFY_RETRY_DELAY_MS apart
     * -- see that block's own comment for why an unpaced, unbounded retry
     * here was a real problem (a CHECKIN storm, not just a busy loop). */
    uint32_t pending_verify_retries = 0;

    for (;;) {
        /* Steps 3+4 (spec §4): checkin, ack handling, wake-success
         * bookkeeping. Looped only when the rollback-sleep gate below
         * blocks sleeping -- see that block's own comment for why re-running
         * this on every such retry (rather than just re-checking the gate)
         * is what lets the failed-wake fallback act as its escape hatch. */
        swarm_checkin_ack_t ack;
        bool got_ack = do_checkin_round(mode, wake_counter, &ack);

        if (got_ack) {
            if (ack.command == SWARM_CHECKIN_CMD_SET_MODE) {
                /* Wake-is-a-boot (spec §4's own section title): persisting
                 * the new mode and rebooting, rather than switching modes
                 * in place, means the very next boot lands back in main.c's
                 * node-paired branch and re-derives everything (radio,
                 * forward_task, and this function's own gating on the new
                 * mode) from a clean boot -- instead of this function
                 * having to unwind and restart its own already-live
                 * scan/forward state in place. */
                esp_err_t serr = swarm_store_set_power_mode((swarm_power_mode_t)ack.arg);
                if (serr != ESP_OK) {
                    /* Code review fix (issue 6): only reboot once the new
                     * mode is actually persisted -- rebooting on an
                     * unpersisted write would come back up still in THIS
                     * mode, having thrown away the reboot for nothing. Not
                     * persisted is not lost: the hub keeps re-sending
                     * SET_MODE on every future checkin until reported/desired
                     * agree (batt_reconcile() is idempotent), so simply not
                     * rebooting this round -- falling through to the normal
                     * bookkeeping/sleep below -- is enough. */
                    ESP_LOGE(TAG, "CHECKIN_ACK: SET_MODE %u -- failed to persist (%s), not rebooting; "
                                  "the hub will re-send this command", ack.arg, esp_err_to_name(serr));
                } else {
                    /* Code review fix (issue 3): a deliberate SET_MODE
                     * reboot rolls back a good PENDING_VERIFY image exactly
                     * like an unconfirmed sleep-triggered reset would (the
                     * bootloader reverts any PENDING_VERIFY image that was
                     * never explicitly confirmed before the next boot) --
                     * so confirm synchronously, right here, before
                     * rebooting, same reasoning as the rollback-sleep rule
                     * just below in this same function. */
                    if (running_image_pending_verify()) {
                        ESP_LOGW(TAG, "CHECKIN_ACK: SET_MODE %u -- running image is PENDING_VERIFY, "
                                      "confirming before rebooting", ack.arg);
                        confirm_health_before_restart("mode change");
                    }
                    ESP_LOGW(TAG, "CHECKIN_ACK: SET_MODE %u -- persisted, rebooting", ack.arg);
                    esp_restart();
                }
            } else if (ack.command == SWARM_CHECKIN_CMD_STAY_AWAKE) {
                ESP_LOGI(TAG, "CHECKIN_ACK: STAY_AWAKE -- waiting up to %us for a parked OTA session",
                         (unsigned)BATT_STAY_AWAKE_CAP_S);
                battery_stay_awake_wait();
            }
            /* NONE: nothing further to do before the bookkeeping below. */
        }

        /* Wake success = a CHECKIN_ACK was received, any command (spec §4
         * step 4). Persisted only when the counter actually changed --
         * avoids an NVS write every single round for no behavioural
         * difference (e.g. already-0 on repeated successes). */
        uint32_t persisted_failed = swarm_store_failed_wakes();
        bool fallback = false;
        uint32_t new_failed = batt_failed_wake_next(persisted_failed, got_ack, &fallback);
        if (new_failed != persisted_failed) {
            esp_err_t ferr = swarm_store_set_failed_wakes(new_failed);
            if (ferr != ESP_OK) {
                ESP_LOGW(TAG, "battery cycle: failed to persist failed-wake counter (%s)",
                         esp_err_to_name(ferr));
            }
        }
        if (fallback) {
            char reason[40];
            snprintf(reason, sizeof(reason), "%" PRIu32 " consecutive failed wakes/rounds", new_failed);
            battery_enter_always_on_fallback(reason);
            return ESP_OK;
        }

        /* Rollback-sleep rule (spec §4, verbatim rationale): "an OTA'd
         * image boots PENDING_VERIFY; the existing node health signal
         * (first delivered reading, or a received ack) confirms it -- both
         * happen within the first wake's window, before the first sleep.
         * If neither happens (hub gone at exactly the wrong moment), the
         * node must not sleep with an unconfirmed image (the next wake's
         * reset would roll back a good image): an unconfirmed image keeps
         * the node awake retrying until confirmed or the failed-wake
         * fallback triggers." The "loop back" above is exactly that
         * retrying: every extra round here also re-runs the failed-wake
         * accounting just above with THIS round's own outcome, so a hub
         * that is truly gone (not just quiet on health signals) still
         * converges on the ALWAYS_ON fallback above rather than spinning
         * forever -- that fallback is the escape hatch this rule leans on.
         *
         * Code review fix (issue 1, blocking): the above escape hatch does
         * NOT cover a reachable hub whose acks keep succeeding while the
         * image never actually leaves PENDING_VERIFY -- e.g.
         * ensure_health_task() failed to start back in swarm_start_node(),
         * or ota_rollback_guard_node_confirm()'s flash write keeps failing
         * (see the companion fix to that function's latch bug). got_ack
         * stays true every round in that case, so batt_failed_wake_next()
         * above never trips its own fallback, and without pacing this
         * spun as fast as one CHECKIN round per network round-trip --
         * roughly 25-40 CHECKINs/second, forever, not merely a busy loop
         * but a radio storm this node's own hub had to absorb. Fixed with
         * its own pace (BATT_PENDING_VERIFY_RETRY_DELAY_MS between passes)
         * and its own bound (BATT_PENDING_VERIFY_MAX_RETRIES passes, ~2
         * minutes total): exhausting it forces the SAME ALWAYS_ON fallback
         * as a truly-gone hub, on the reasoning that an image that cannot
         * confirm itself after two full minutes of successful hub contact
         * is not going to start doing so by spinning faster. */
        if (running_image_pending_verify()) {
            pending_verify_retries++;
            if (pending_verify_retries > BATT_PENDING_VERIFY_MAX_RETRIES) {
                char reason[56];
                snprintf(reason, sizeof(reason), "PENDING_VERIFY unconfirmed after %" PRIu32 " retries",
                         pending_verify_retries);
                battery_enter_always_on_fallback(reason);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "battery cycle: running image is still PENDING_VERIFY (retry %" PRIu32 "/%u); "
                          "not sleeping, waiting %us before retrying the checkin",
                     pending_verify_retries, (unsigned)BATT_PENDING_VERIFY_MAX_RETRIES,
                     (unsigned)(BATT_PENDING_VERIFY_RETRY_DELAY_MS / 1000));
            vTaskDelay(pdMS_TO_TICKS(BATT_PENDING_VERIFY_RETRY_DELAY_MS));
            continue;
        }

        /* Step 6 (spec §4): sleep for the remainder of the period.
         * awake_ms is measured from this function's own entry, per the
         * brief -- batt_sleep_us()'s precondition (never ALWAYS_ON) is
         * already satisfied by the early return above, since `mode` cannot
         * have changed since then (the only way it could is SET_MODE,
         * which reboots immediately, above, or the fallback, which returns
         * immediately, above -- neither falls through to here). */
        int64_t awake_us = esp_timer_get_time() - wake_start_us;
        uint32_t awake_ms = (uint32_t)(awake_us / 1000);
        ESP_LOGI(TAG, "battery cycle: sleeping (mode=%u wake=%" PRIu32 " awake=%" PRIu32 "ms)",
                 mode, wake_counter, awake_ms);
        esp_deep_sleep(batt_sleep_us(mode, awake_ms));
        /* esp_deep_sleep() never returns -- the next execution of this
         * device is a fresh boot through app_main(), not a return here. */
    }
}

static esp_err_t radio_only_wifi_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t empty_cfg = { 0 };
    err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
    if (err != ESP_OK) return err;

    char name[16];
    app_config_hub_name(name);
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, name, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(name);
    ap_cfg.ap.ssid_hidden = 1;
    ap_cfg.ap.max_connection = 0;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    return esp_wifi_start();
}

esp_err_t swarm_start_node(void)
{
    /* The LMK isn't needed here: espnow_link_init() below re-adds the
     * stored hub peer (MAC + LMK + channel-follows-radio) on its own for a
     * SWARM_ROLE_NODE device, so this call only needs the MAC and channel. */
    uint8_t hub_mac[6], hub_ch;
    if (!swarm_store_hub(hub_mac, NULL, &hub_ch)) {
        ESP_LOGE(TAG, "swarm_start_node: no stored hub; caller must check swarm_store_hub() first");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_hub_mac, hub_mac, 6);

    /* Must be armed BEFORE espnow_link_init() below hands node_rx_cb its
     * first frame -- an OTA_BEGIN could arrive at any point after that call
     * returns. node_ota_recv_init() only creates a queue + task (RAM-only,
     * no flash/network I/O), so it's cheap and safe this early. Idempotent;
     * a no-op on every boot that isn't currently receiving a push. */
    esp_err_t oerr = node_ota_recv_init();
    if (oerr != ESP_OK) {
        ESP_LOGE(TAG, "swarm_start_node: node_ota_recv_init failed (%s); node OTA pushes will not "
                      "be receivable this boot", esp_err_to_name(oerr));
    }

    /* Same "must exist before node_rx_cb/forward_task can call it" reasoning
     * as node_ota_recv_init() above -- see ensure_health_task()'s own
     * comment for why this is eager rather than the lazy-on-first-use
     * pattern used elsewhere in this file/pairing.c. Failure here is logged,
     * not fatal: the node still forwards/pairs/receives OTA pushes fine,
     * only the rollback-guard confirmation signal would never fire, so a
     * genuinely OTA'd-and-healthy node could still roll back on its next
     * reboot -- serious, but not a reason to refuse to start as a node
     * entirely (which would itself be a worse outcome: no forwarding at
     * all). */
    esp_err_t herr = ensure_health_task();
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "swarm_start_node: ensure_health_task failed (%s); the OTA rollback-guard "
                      "health signal will never fire this boot", esp_err_to_name(herr));
    }

    /* Same "must exist before node_rx_cb can call it" reasoning as the two
     * queues/tasks just above -- see s_checkin_ack_queue's own comment. */
    if (!s_checkin_ack_queue) s_checkin_ack_queue = xQueueCreate(1, sizeof(swarm_checkin_ack_t));
    if (!s_checkin_ack_queue) {
        ESP_LOGE(TAG, "swarm_start_node: failed to create CHECKIN_ACK queue; CHECKIN acks will "
                      "never be delivered this boot");
    }

    esp_err_t err = radio_only_wifi_start();
    if (err != ESP_OK) return err;

    /* Order matters: espnow_link_init() below sets the regulatory domain
     * (esp_wifi_set_country_code()), which can move the radio's current
     * channel as a side effect (see the comment there). The stored hub
     * channel is restored AFTER espnow_link_init() returns, precisely so
     * that restore is the last word on which channel the radio ends up
     * on -- do not reorder these two calls. */
    err = espnow_link_init(node_rx_cb);
    if (err != ESP_OK) return err;

    /* Country inheritance (PlanV1 3.3): espnow_link_init() just set the
     * regulatory domain to the compile-time CONFIG_PLANTHUB_WIFI_COUNTRY
     * default. If this node has previously learned a different country
     * from the hub's PAIR_ACK (protocol v2+), re-apply THAT here instead --
     * every boot, not just the one right after pairing -- so a resync's
     * channel sweep (pairing_node_resync_channel(), below this in the
     * boot sequence via forward_task()) can actually reach channels 12-13
     * if the hub's domain allows them, rather than silently reverting to
     * the compile-time default on every restart. Absent (this node paired
     * under v1, or was factory-reset) just means "nothing to reapply" --
     * the compile-time default espnow_link_init() already set stands, same
     * as a pre-v2 node. Same MANUAL policy reasoning as pairing.c: this
     * device never associates, so ieee80211d_enabled stays false always. */
    char hub_cc[3];
    if (swarm_store_hub_country(hub_cc)) {
        esp_err_t cc_err = esp_wifi_set_country_code(hub_cc, false);
        if (cc_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to reapply learned hub country %s: %s",
                     hub_cc, esp_err_to_name(cc_err));
        } else {
            ESP_LOGI(TAG, "reapplied learned hub country %s", hub_cc);
        }
    }

    err = espnow_link_set_channel(hub_ch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore channel %u: %s", hub_ch, esp_err_to_name(err));
    }
    /* Defensive re-assert: confirm the restore actually landed rather than
     * assuming it did. Should be a no-op given the ordering above, but
     * costs nothing to verify and self-correct if some other side effect
     * (country config or otherwise) ever moves the channel again. */
    uint8_t actual_ch = espnow_link_channel();
    if (actual_ch != hub_ch) {
        ESP_LOGW(TAG, "channel mismatch after restore: stored=%u actual=%u, retrying", hub_ch, actual_ch);
        espnow_link_set_channel(hub_ch);
        actual_ch = espnow_link_channel();
    }

    swarm_buf_init(&s_buf);   /* static, already zero at boot -- explicit for clarity */
    s_fwd_queue = xQueueCreate(SWARM_FWD_QUEUE_LEN, sizeof(swarm_reading_t));
    if (!s_fwd_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(forward_task, "swarm_fwd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create forward task");
        return ESP_ERR_NO_MEM;
    }

    /* M7 Task 5 (spec §4/§6): started unconditionally for every paired
     * node, not only battery-configured ones -- see the task's own comment
     * for why an ALWAYS_ON node needs this too. Failure here is logged, not
     * fatal to starting as a node: forwarding/pairing/OTA all still work,
     * only a desired-mode change from the hub would go undelivered until
     * this node's next reboot. */
    if (xTaskCreate(always_on_checkin_task, "swarm_ao_checkin", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create always-on checkin task; hub-initiated mode changes "
                      "will not be delivered until this node next reboots");
    }

    err = esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE, on_sensor_update, NULL);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "node started: hub=" MACSTR " stored_channel=%u actual_channel=%u",
             MAC2STR(hub_mac), hub_ch, actual_ch);
    return ESP_OK;
}

/* ---------------- Node side: searching for a hub (unpaired) ---------------- */

#define SWARM_PAIR_SEARCH_TIMEOUT_S 120

/* Polls pairing_node_state() rather than blocking on a semaphore signalled
 * by pairing_node_start()'s own task: that task's only externally-visible
 * outcome is this polled state (see pairing.h), so a small poll loop is
 * the simplest correct way to notice PAIR_OK/PAIR_FAILED and act on it. */
static void pair_watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        pairing_state_t st = pairing_node_state();
        if (st == PAIR_OK) {
            ESP_LOGI(TAG, "pairing succeeded; clearing pair-failed flag and rebooting as a paired node");
            swarm_store_set_pair_failed(false);
            esp_restart();
        } else if (st == PAIR_FAILED) {
            ESP_LOGW(TAG, "pairing failed/timed out; marking pair-failed and rebooting into the portal");
            swarm_store_set_pair_failed(true);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t swarm_start_node_search(void)
{
    /* Same radio-only bring-up as swarm_start_node(), but this device has
     * no stored hub yet -- it's actively looking for one, so there is no
     * channel to restore and node_rx_cb (which just forwards to
     * pairing_handle_frame) is exactly what's needed to receive PAIR_ACK. */
    esp_err_t err = radio_only_wifi_start();
    if (err != ESP_OK) return err;

    err = espnow_link_init(node_rx_cb);
    if (err != ESP_OK) return err;

    err = pairing_node_start(SWARM_PAIR_SEARCH_TIMEOUT_S);
    if (err != ESP_OK) return err;

    if (xTaskCreate(pair_watch_task, "swarm_pairwatch", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create pair-watch task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "searching for a hub (up to %ds)...", SWARM_PAIR_SEARCH_TIMEOUT_S);
    return ESP_OK;
}
