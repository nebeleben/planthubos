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
        bool ota_pending = node_ota_pending_for(item.mac);
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
