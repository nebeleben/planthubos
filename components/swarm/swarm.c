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
#include "bridge_table.h"
#include "bridge_cmd.h"
#include "swarm_rules.h"
#include "radio_role.h"
#include "swarm_buf.h"
#include "batt_cycle.h"
#include "node_ota.h"
#include "node_ota_recv.h"
#include "espnow_link.h"
#include "pairing.h"
#include "data_core.h"
#include "registry.h"
#include "capability.h"
#include "mibeacon.h"
#include "app_config.h"
#include "rules.h"
/* M7 Task 5: a zigbee-role node's forwarder pulls the joined-device table
 * and registers itself as zigbee.c's device/status observer -- one-way
 * (swarm -> zigbee); zigbee.c itself must never include anything from this
 * component (see zigbee.h's own top comment on the CMake-cycle this avoids). */
#include "zigbee.h"
/* M7 Task 6: a zigbee-role node's command task is the ONLY thing on a
 * bridge node that ever calls into the actor queue (actor_request()/
 * actor_service()) -- the hub-side hub_rx_cb path never touches it. Same
 * one-way dependency shape as zigbee.h just above. */
#include "actor.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
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
    /* M7 Task 4: the node's own last-reported radio role (RADIO_ROLE_*,
     * radio_role_str.h), learned from PAIR_REQ.radio_role (pairing.c, via
     * swarm_note_node_radio()) or COORD_STATUS (Task 7) -- same
     * "valid until proven otherwise" shape as reported_mode/
     * reported_mode_valid above. */
    uint8_t  reported_radio_role;
    bool     reported_radio_valid;
    /* M7 Task 4: per-node NODE_CONFIG sequence counter, incremented by
     * swarm_send_node_config() on every send so the NODE_CONFIG_ACK this
     * node replies with can be correlated to the request it answers in the
     * log (hub_rx_cb's NODE_CONFIG_ACK branch). Persists only for this
     * boot, same as every other field in this RAM-only struct. */
    uint16_t cfg_seq;
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

/* M7 Task 4: copy of record_checkin_mode() above, writing the node's
 * self-reported radio role instead of its power mode -- see
 * swarm_note_node_radio()'s doc comment in swarm.h for callers and the
 * same "a miss just means no slot yet" reasoning. Unlike record_checkin_mode(),
 * this can be called for a node BEFORE it has ever sent a READING/CHECKIN
 * this boot (its first-ever call site is pairing.c's PAIR_REQ handling,
 * which runs before record_stat() has necessarily created a slot for a
 * still-unadopted node) -- so a miss here is the expected common case for a
 * freshly pairing node, not just the defensive corner case it is for
 * record_checkin_mode(). */
static void record_reported_radio(const uint8_t mac[6], uint8_t r)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0) {
            s_stats[i].reported_radio_role = r;
            s_stats[i].reported_radio_valid = true;
            break;
        }
    }
    xSemaphoreGive(s_stats_mutex);
}

void swarm_note_node_radio(const uint8_t mac[6], uint8_t r)
{
    if (!mac || !s_stats_mutex) return;
    record_reported_radio(mac, r);
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

/* See swarm.h's doc comment. Same shape as swarm_node_reported_mode() just
 * above, over reported_radio_role/reported_radio_valid instead. */
bool swarm_node_reported_radio(const uint8_t mac[6], uint8_t *role_out)
{
    if (!mac || !role_out || !s_stats_mutex) return false;
    bool found = false;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0 && s_stats[i].reported_radio_valid) {
            *role_out = s_stats[i].reported_radio_role;
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
    /* Wake the rules engine (spec §4 "Triggers") for the swarm-ingest leg,
     * same as ble_collector.c's gap_event() does for the hub's own direct
     * BLE reception -- see that call site's comment for why a plain
     * event-group bit set is safe to add here despite ingest_reading()
     * running on the ESP-NOW receive callback (WiFi driver task), the exact
     * context this file's own comment above already established
     * data_core_submit_from() itself is safe to call from directly (short,
     * bounded, never blocks). rules_notify_value_update() is safe before
     * rules_init() has run (rules.h). */
    rules_notify_value_update();
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

/* M7 Task 4: encodes and sends a NODE_CONFIG to `mac`, assigning it a fresh
 * per-node sequence number (node_stat_t.cfg_seq). Only ever called from a
 * task context (checkin_task, below) -- see swarm.h's doc comment for why.
 * Creates this node's RAM stats slot on demand if it doesn't have one yet
 * (unlike record_checkin_mode()/record_reported_radio(), which only ever
 * update an EXISTING slot) -- a node targeted by an operator's
 * POST /api/v1/nodes/{MAC12} {"radio_role":...} may never have transmitted
 * this boot, but still needs a durable per-node cfg_seq counter that
 * survives across repeated calls for the same node this boot, same
 * slot-creation shape as record_stat(). */
esp_err_t swarm_send_node_config(const uint8_t mac[6], radio_role_t r)
{
    if (!mac || !s_stats_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    int idx = -1, free_idx = -1;
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0) { idx = i; break; }
        if (!s_stats[i].in_use && free_idx < 0) free_idx = i;
    }
    if (idx < 0) idx = free_idx;
    if (idx < 0) {
        xSemaphoreGive(s_stats_mutex);
        return ESP_ERR_NO_MEM;
    }
    if (!s_stats[idx].in_use) {
        s_stats[idx].in_use = true;
        memcpy(s_stats[idx].mac, mac, 6);
        s_stats[idx].frames_rx = 0;
    }
    uint16_t seq = ++s_stats[idx].cfg_seq;
    xSemaphoreGive(s_stats_mutex);

    swarm_node_config_t cfg = { .seq = seq, .radio_role = (uint8_t)r };
    uint8_t buf[16];
    size_t n = swarm_encode_node_config(&cfg, buf, sizeof(buf));
    if (n == 0) {
        ESP_LOGE(TAG, "swarm_send_node_config for " MACSTR ": failed to encode", MAC2STR(mac));
        return ESP_ERR_INVALID_STATE;
    }
    return espnow_link_send(mac, buf, n);
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
 * sends".
 *
 * config_only (M7 Task 4): true for an item posted by
 * swarm_request_node_config() (an operator's radio-role change, wanting an
 * immediate push) rather than decoded from a real CHECKIN frame -- `checkin`
 * is meaningless in that case. checkin_task() sends such an item a bare
 * NODE_CONFIG and nothing else (no CHECKIN_ACK, no batt_reconcile()): the
 * node did not check in, so acking a CHECKIN it never sent would be a lie. */
typedef struct {
    uint8_t         mac[6];
    bool            config_only;
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

        if (item.config_only) {
            /* An operator-triggered push (api_v1.c's node_update_post(), via
             * swarm_request_node_config()): send the node's CURRENT desired
             * radio role right now, regardless of what it last reported --
             * the operator asked for it now, not "eventually, once this
             * differs from what we last heard". */
            radio_role_t desired_radio = swarm_store_node_desired_radio(item.mac);
            esp_err_t rerr = swarm_send_node_config(item.mac, desired_radio);
            if (rerr != ESP_OK) {
                ESP_LOGW(TAG, "NODE_CONFIG (requested) -> " MACSTR " failed (%s)",
                         MAC2STR(item.mac), esp_err_to_name(rerr));
            } else {
                ESP_LOGI(TAG, "NODE_CONFIG (requested) -> " MACSTR ": radio_role=%s",
                         MAC2STR(item.mac), radio_role_str(desired_radio));
            }
            continue;
        }

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

        /* M7 Task 4: right AFTER the CHECKIN_ACK above, not instead of it --
         * a battery node is only reachable during this brief awake window,
         * so this is the one reliable moment to also push a radio-role
         * change onto it (an always-on node's periodic checkin gives the
         * same opportunity every cycle). Sent whenever this node's desired
         * radio role differs from what it last reported, OR it has never
         * reported one at all and desired isn't the RADIO_ROLE_BLE default
         * -- same "pending" condition swarm_node_list_json() computes for
         * its own radio_role_pending field, kept in sync with it
         * deliberately (see that function's own comment for why unknown is
         * treated as "assume BLE" rather than "assume compliant"). A send
         * failure here is logged and dropped, not retried inline: the next
         * checkin (or a later operator-triggered swarm_request_node_config())
         * retries it, same self-healing shape as the CHECKIN_ACK send above. */
        radio_role_t desired_radio = swarm_store_node_desired_radio(item.mac);
        uint8_t reported_radio_byte;
        bool have_reported_radio = swarm_node_reported_radio(item.mac, &reported_radio_byte);
        bool radio_pending = have_reported_radio ? (reported_radio_byte != (uint8_t)desired_radio)
                                                  : (desired_radio != RADIO_ROLE_BLE);
        if (radio_pending) {
            esp_err_t rerr = swarm_send_node_config(item.mac, desired_radio);
            if (rerr != ESP_OK) {
                ESP_LOGW(TAG, "NODE_CONFIG -> " MACSTR " failed (%s), dropped -- reconciliation "
                              "self-heals next checkin", MAC2STR(item.mac), esp_err_to_name(rerr));
            } else {
                ESP_LOGI(TAG, "NODE_CONFIG -> " MACSTR ": radio_role=%s",
                         MAC2STR(item.mac), radio_role_str(desired_radio));
            }
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

esp_err_t swarm_request_node_config(const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!s_checkin_queue) return ESP_ERR_INVALID_STATE;

    /* Fix round 1: skip queuing entirely when this node's desired radio
     * role already matches what it last reported -- an idempotent repeat
     * of an already-applied POST /api/v1/nodes/{MAC12} {"radio_role":...}
     * (or one that raced a DONE ack that already cleared this) then sends
     * nothing over the air and never reaches node_config_task() at all, so
     * it can never trigger a needless reboot there either. A node that has
     * never reported a radio role this boot falls back to comparing against
     * RADIO_ROLE_BLE, same "unknown treated as the default" reasoning as
     * every other reported/desired comparison in this file
     * (swarm_node_list_json()'s radio_role_pending, checkin_task()'s own
     * radio_pending). */
    radio_role_t desired = swarm_store_node_desired_radio(mac);
    uint8_t reported_byte;
    bool have_reported = swarm_node_reported_radio(mac, &reported_byte);
    bool already_applied = have_reported ? (reported_byte == (uint8_t)desired)
                                          : (desired == RADIO_ROLE_BLE);
    if (already_applied) return ESP_OK;

    checkin_item_t item;
    memset(&item, 0, sizeof(item));
    memcpy(item.mac, mac, 6);
    item.config_only = true;
    if (xQueueSend(s_checkin_queue, &item, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* ---------------- Hub side: bridge table + ingest (M7 Task 7) ----------------
 *
 * s_bridges is the hub's own bookkeeping of every zigbee-role node it has
 * heard from: that node's last coordinator status, and every end device it
 * has announced. Distinct from s_stats[] above (generic per-node ESP-NOW
 * stats, every role) and from data_core's registry (capability readings,
 * keyed by device_id_t, no notion of "which bridge node reports this") --
 * a zigbee device lives in both, cross-referenced by (kind, addr).
 *
 * s_bridges_mutex guards every access: bridge_task (below) is its only
 * writer under ordinary operation, serialised off s_bridge_queue same as
 * checkin_task, but swarm_forget_node_stats() (called from the forget HTTP
 * handler's task, api_v1.c) also writes it directly and synchronously
 * (forgetting a node must take effect immediately, not queue behind
 * whatever bridge_task is mid-processing) -- so unlike s_bridges itself,
 * this needs an explicit mutex the brief's pseudocode doesn't spell out.
 * ONLY ever held for a short, bounded, in-memory scan/mutation -- never
 * across a blocking send (see request_resync()'s own "release, then send"
 * discipline) or file I/O (see bridge_save()/bridge_load() and
 * s_bridge_io_mutex below): swarm_zb_dispatch() (ble_collector.c's
 * adv_decoder_task, the BLE decoder task) also takes this mutex for a
 * quick lookup and must never be made to wait on a flash write. */
static bridge_table_t     s_bridges;
static SemaphoreHandle_t  s_bridges_mutex;

/* M7 Task 8: the hub's command router -- tracks the one outstanding
 * SWARM_MSG_COMMAND this hub may have in flight to each bridge node at a
 * time (PERMIT_JOIN/DEVICE_REMOVE/DEVICE_RENAME/ACTUATE/RESYNC), from
 * submission through retry, ack correlation and TTL expiry. Pure C
 * (bridge_cmd.h/.c) -- s_router itself needs NO mutex of its own: unlike
 * s_bridges above (which swarm_forget_node_stats() also writes, from a
 * different task), bridge_task is s_router's ONLY caller, on every code
 * path -- see bridge_task() below and this file's swarm_bridge_permit/
 * remove/rename()/swarm_zb_dispatch(), all of which only ever POST an item
 * onto s_bridge_queue rather than touch s_router directly. */
static bridge_router_t s_router;

_Static_assert(BRIDGE_MAX_NODES == SWARM_MAX_NODES,
               "bridge_table.h's BRIDGE_MAX_NODES duplicates SWARM_MAX_NODES's value "
               "(pure-C header, cannot include swarm_store.h) -- keep them equal by hand");

#define BRIDGE_STORE_PATH     "/storage/bridges.bin"
#define BRIDGE_STORE_TMP_PATH "/storage/bridges.tmp"
/* Sized the same way zb_store.h's ZB_STORE_IMAGE_MAX is: header/per-node
 * overhead plus BRIDGE_MAX_DEVICES devices at a generous per-device ceiling
 * (see bridge_table.c's own BRIDGE_DEV_ENCODE_MAX comment for the real,
 * tighter worst case -- this stays a round, obviously-sufficient number). */
#define BRIDGE_STORE_IMAGE_MAX (16 + BRIDGE_MAX_NODES * (16 + BRIDGE_MAX_DEVICES * 96))

/* M7 Task 7 fix round 1 (critical #1): BRIDGE_STORE_IMAGE_MAX is ~9.3 KB --
 * far too large for either call site's stack (bridge_save() runs on
 * bridge_task, a 4096 B stack; bridge_load() runs once at boot, on
 * swarm_start_main()'s caller's stack, smaller still) -- a stack-local
 * array of this size (this function's original shape) is a guaranteed
 * overflow on real hardware. static instead: no heap.
 *
 * Fix round 1 addendum (from Task 8's review): this buffer, and the file
 * I/O around it, must NOT run while s_bridges_mutex is held -- Task 8's
 * swarm_zb_dispatch() (ble_collector.c's adv_decoder_task, the BLE decoder
 * task) also takes s_bridges_mutex for a quick lookup and must never be
 * made to wait on a flash write. s_bridge_io_mutex is this buffer's OWN
 * lock, serialising bridge_save()/bridge_load() against each other (so two
 * saves, or a save racing the one boot-time load, can never interleave
 * their writes into this shared buffer) entirely independently of
 * s_bridges_mutex.
 *
 * LOCK ORDER, wherever both are ever held together (bridge_save() below is
 * the only such place): s_bridge_io_mutex OUTER, s_bridges_mutex INNER.
 * Never the other way around, anywhere in this file -- reversing it would
 * risk a classic lock-order deadlock against swarm_zb_dispatch() or
 * bridge_task's own table-mutating cases if either of those ever grew a
 * reason to also take s_bridge_io_mutex while already holding
 * s_bridges_mutex. */
static uint8_t          s_bridge_io_buf[BRIDGE_STORE_IMAGE_MAX];
static SemaphoreHandle_t s_bridge_io_mutex;

/* I2 fix (save storms): the CRC32 of the last image actually written to
 * flash by bridge_save(), so an idempotent re-announce/resync round (the
 * common case once the table has converged) skips the tmp+rename entirely
 * instead of rewriting a byte-identical image. Guarded by s_bridge_io_mutex
 * -- the same lock that already serialises every access to s_bridge_io_buf
 * and every bridge_save() call, so no separate lock is needed here. */
static uint32_t s_bridge_saved_crc;
static bool     s_bridge_saved_crc_valid;

/* I2 fix (save storms): coalesces bridge_save() calls across a burst of
 * queue items (a RESYNC replay can post up to ZB_STORE_MAX_DEVICES ANNOUNCE
 * frames back-to-back) instead of one flash rewrite per item. Set true by
 * bridge_task's own ANNOUNCE/GONE/STATUS-with-change cases and by
 * swarm_forget_node_stats() (a different task) -- both already hold
 * s_bridges_mutex at the point they set it, which is this flag's guard.
 * bridge_task itself decides WHEN to act on it (queue-drained or the 2 s
 * cap), in its main loop below. */
static bool     s_bridge_dirty;
static uint32_t s_bridge_dirty_since_s;

/* Marks the bridge table dirty; caller must hold s_bridges_mutex (every
 * call site already does, for the table mutation that makes this dirty in
 * the first place). now_s is only recorded the first time the flag flips
 * from clean to dirty, so a steady stream of further changes doesn't keep
 * pushing the 2 s deadline back. */
static void mark_bridge_dirty(uint32_t now_s)
{
    if (!s_bridge_dirty) s_bridge_dirty_since_s = now_s;
    s_bridge_dirty = true;
}

/* Same tmp+rename discipline as zigbee.c's zb_store_save() (that file's own
 * comment has the full power-loss-atomicity reasoning); this omits the
 * explicit fsync() that file adds -- the bridge table is reconstructible
 * from the next RESYNC round-trip with every node, unlike the zigbee
 * joined-device table, which has no such second source of truth if a torn
 * write lost it.
 *
 * Callers must NOT hold s_bridges_mutex when calling this (see the lock-
 * order comment above s_bridge_io_buf) -- bridge_task's cases and
 * swarm_forget_node_stats() both mutate the table under s_bridges_mutex,
 * release it, and only THEN call this; this function re-takes
 * s_bridges_mutex itself, briefly, purely to serialise a stable snapshot
 * into s_bridge_io_buf, and releases it again before touching the
 * filesystem. */
static void bridge_save(void)
{
    xSemaphoreTake(s_bridge_io_mutex, portMAX_DELAY);

    xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
    size_t len = bridge_table_serialize(&s_bridges, s_bridge_io_buf, sizeof s_bridge_io_buf);
    xSemaphoreGive(s_bridges_mutex);

    if (len == 0) {
        ESP_LOGE(TAG, "bridge table serialize failed; not persisted");
        xSemaphoreGive(s_bridge_io_mutex);
        return;
    }

    /* I2 fix: skip the tmp+rename entirely when this image is byte-
     * identical to the last one actually written -- an idempotent
     * re-announce, a resync round after the table has already converged,
     * or a forget that nets out to no change all hit this. */
    uint32_t crc = esp_crc32_le(0, s_bridge_io_buf, len);
    if (s_bridge_saved_crc_valid && crc == s_bridge_saved_crc) {
        xSemaphoreGive(s_bridge_io_mutex);
        return;
    }

    FILE *f = fopen(BRIDGE_STORE_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for write (errno=%d); a reboot now would lose the "
                      "bridge table", BRIDGE_STORE_TMP_PATH, errno);
        xSemaphoreGive(s_bridge_io_mutex);
        return;
    }
    size_t wrote = fwrite(s_bridge_io_buf, 1, len, f);
    if (wrote != len || fclose(f) != 0) {
        ESP_LOGW(TAG, "short write or close failure persisting the bridge table to %s",
                 BRIDGE_STORE_TMP_PATH);
        remove(BRIDGE_STORE_TMP_PATH);
        xSemaphoreGive(s_bridge_io_mutex);
        return;
    }
    if (rename(BRIDGE_STORE_TMP_PATH, BRIDGE_STORE_PATH) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno=%d); a reboot now would lose the bridge table",
                 BRIDGE_STORE_TMP_PATH, BRIDGE_STORE_PATH, errno);
        remove(BRIDGE_STORE_TMP_PATH);
    } else {
        s_bridge_saved_crc = crc;
        s_bridge_saved_crc_valid = true;
    }
    xSemaphoreGive(s_bridge_io_mutex);
}

/* Called once, from swarm_start_main(), before ensure_bridge_task() ever
 * creates s_bridges_mutex or starts bridge_task -- so s_bridges itself
 * needs no lock here (mirrors zb_store_load()'s own comment on the same
 * point). Still takes s_bridge_io_mutex around s_bridge_io_buf, even though
 * nothing else can be running yet: keeps "every access to this buffer holds
 * this mutex" an unconditional invariant rather than one with a boot-time
 * exception to remember. s_bridge_io_mutex itself is created in
 * swarm_start_main() BEFORE this is called (unlike s_bridges_mutex, which
 * ensure_bridge_task() creates lazily afterwards) -- see that function. */
static void bridge_load(void)
{
    bridge_table_init(&s_bridges);

    xSemaphoreTake(s_bridge_io_mutex, portMAX_DELAY);
    FILE *f = fopen(BRIDGE_STORE_PATH, "rb");
    if (!f) {
        xSemaphoreGive(s_bridge_io_mutex);
        ESP_LOGI(TAG, "%s: not present (first boot, or no bridge node has announced a device yet)",
                 BRIDGE_STORE_PATH);
        return;
    }
    size_t n = fread(s_bridge_io_buf, 1, sizeof s_bridge_io_buf, f);
    fclose(f);
    bool ok = bridge_table_deserialize(&s_bridges, s_bridge_io_buf, n);
    xSemaphoreGive(s_bridge_io_mutex);

    if (!ok) {
        ESP_LOGW(TAG, "%s: %u byte(s) unreadable (bad magic, version, length or count); "
                      "starting this boot with an empty bridge table", BRIDGE_STORE_PATH, (unsigned)n);
        bridge_table_init(&s_bridges);
        return;
    }
    ESP_LOGI(TAG, "bridge table restored from before this boot");
}

/* One item per accepted v4 bridge frame, queued by hub_rx_cb and drained by
 * bridge_task -- same "callback queues, task does the real work" shape as
 * checkin_item_t/checkin_task above (hub_rx_cb must never block, and
 * data_core_find_or_create_index()/espnow_link_send() below both can). type
 * is one of the SWARM_MSG_* values this ingest handles; the matching union
 * member is the frame this hub_rx_cb already decoded (decoding in the
 * callback, not the task, means a malformed frame from a paired-but-buggy
 * or spoofed-MAC sender is dropped immediately rather than filling the
 * queue with garbage).
 *
 * M7 Task 8 adds a second, purely-internal producer: BRIDGE_ITEM_SUBMIT,
 * posted by request_resync() (below), swarm_bridge_permit/remove/rename()
 * and swarm_zb_dispatch() -- never decoded off the wire, so its value
 * (100) is deliberately well outside the SWARM_MSG_* range (1-20,
 * swarm_frame.h) those come from. Routing it through this SAME queue/task,
 * rather than a second one, is what makes bridge_task the router's only
 * caller: every path that wants to submit a command funnels through here. */
#define BRIDGE_ITEM_SUBMIT 100

/* M7 zigbee bridge follow-up: a third, also purely-internal producer --
 * posted by hub_rx_cb (via post_bridge_flush(), below request_resync())
 * for every accepted frame from a known node (CHECKIN, the v4 bridge
 * frames, POLL, acks). Same "well outside the SWARM_MSG_* range" reasoning
 * as BRIDGE_ITEM_SUBMIT's own comment -- this is never decoded off the
 * wire. bridge_task's FLUSH case (below) uses it as the trigger to push
 * mac's pending, not-yet-accepted command out right now (see
 * bridge_cmd_peek_pending()/bridge_cmd_mark_sent(), bridge_cmd.h) rather
 * than wait for the periodic retry -- the bench finding behind this is
 * that a zigbee-role bridge's WiFi receive window is only reliably open
 * right this soon after the node's own transmit. */
#define BRIDGE_ITEM_FLUSH 101

typedef struct {
    uint8_t          op;             /* SWARM_CMD_* */
    swarm_dev_addr_t dev;
    uint16_t         arg;
    char             name[SWARM_DEV_NAME_MAX];
    uint8_t          name_len;
    uint32_t         ttl_s;
    int              actor_dev_idx;  /* -1: no actor to report to (every op but ACTUATE) */
    uint8_t          actor_action;
    uint16_t         actor_param;
    bool             want_result;    /* true: a synchronous swarm_bridge_permit/remove/rename()
                                       * caller is waiting on s_submit_done for s_submit_result --
                                       * see submit_and_wait() below. False for request_resync()'s
                                       * own fire-and-forget posts and for swarm_zb_dispatch()'s
                                       * ACTUATE posts (which report a submit failure themselves,
                                       * through actor_dev_idx, rather than blocking on an answer). */
} bridge_submit_t;

typedef struct {
    uint8_t mac[6];
    int     type;
    /* I4 fix: esp_timer_get_time() at the moment hub_rx_cb decoded this
     * item (BEFORE it sat on s_bridge_queue waiting for bridge_task) -- the
     * SWARM_MSG_MEASUREMENT case adds "time since receipt" on top of the
     * frame's own age_s to get the reading's TOTAL age before deciding
     * whether to submit it. Set for every item, not just MEASUREMENT, so
     * there is one uniform place doing it (BRIDGE_ITEM_SUBMIT posts, which
     * originate hub-side rather than off the wire, don't use this field). */
    int64_t recv_us;
    union {
        swarm_device_announce_t ann;
        swarm_device_gone_t     gone;
        swarm_measurement_t     meas;
        swarm_coord_status_t    status;
        swarm_command_ack_t     ack;
        bridge_submit_t         submit;
    } u;
} bridge_item_t;

#define BRIDGE_QUEUE_LEN 16

static QueueHandle_t s_bridge_queue;
static TaskHandle_t  s_bridge_task;

/* M7 Task 8: signals a synchronous swarm_bridge_permit/remove/rename()
 * call that bridge_task has finished processing its BRIDGE_ITEM_SUBMIT and
 * s_submit_result now holds the answer -- see submit_and_wait() below.
 * s_submit_call_mutex serialises concurrent callers of that function (at
 * most one synchronous submit-and-wait may be outstanding at a time, so a
 * second caller's own wait can never be satisfied by the wrong give()) --
 * it does NOT protect s_router or s_bridges, which stay bridge_task-only/
 * s_bridges_mutex-guarded exactly as before. Both created in
 * ensure_bridge_task(), alongside s_bridge_queue/s_bridges_mutex. */
static SemaphoreHandle_t s_submit_done;
static SemaphoreHandle_t s_submit_call_mutex;
static esp_err_t         s_submit_result;

/* Hub -> bridge node, unicast, encrypted (already-adopted peer). Queues a
 * one-off SWARM_CMD_RESYNC so a zigbee bridge re-announces every device it
 * currently knows about -- used both right after boot (every bridge node
 * this hub has a stored status for) and whenever a live COORD_STATUS
 * disagrees with what this hub's own bridge table currently holds (see
 * bridge_task()'s COORD_STATUS case below). M7 Task 8: routed through the
 * same BRIDGE_ITEM_SUBMIT path as every other command this hub sends, so
 * bridge_task's router (s_router) is the ONE place a seq is assigned and a
 * frame is actually put on the wire -- this function no longer encodes or
 * calls espnow_link_send() itself. Safe to call from bridge_task's own
 * task (the COORD_STATUS/MEASUREMENT cases below -- it only enqueues,
 * never blocks) or from swarm_start_main()'s startup task (the post-boot
 * resync sweep). Best-effort/fire-and-forget: a full queue, or a node that
 * already has a command in flight, just means this particular RESYNC
 * request is dropped -- the next mismatched COORD_STATUS (or this node's
 * next boot) asks again. */
static void request_resync(const uint8_t mac[6])
{
    bridge_item_t item;
    memset(&item, 0, sizeof(item));
    memcpy(item.mac, mac, 6);
    item.type = BRIDGE_ITEM_SUBMIT;
    item.u.submit.op = SWARM_CMD_RESYNC;
    item.u.submit.ttl_s = 30;
    item.u.submit.actor_dev_idx = -1;
    if (!s_bridge_queue || xQueueSend(s_bridge_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RESYNC -> " MACSTR ": no bridge task/queue full, dropping", MAC2STR(mac));
    }
}

/* M7 zigbee bridge follow-up: posts a BRIDGE_ITEM_FLUSH for mac -- called
 * from hub_rx_cb (WiFi driver task) for every accepted frame from a known
 * node (CHECKIN, the v4 bridge frames, POLL, acks), right after that
 * frame's own existing handling. Same "callback queues, task does the real
 * work" discipline as every other hub_rx_cb producer in this file:
 * xQueueSend is non-blocking, and a momentarily full queue just drops this
 * one flush -- best-effort, since this node's next frame (POLL included,
 * at most ~2 s away on a zigbee-role node) tries again. */
static void post_bridge_flush(const uint8_t mac[6])
{
    if (!s_bridge_queue) return;
    bridge_item_t item;
    memset(&item, 0, sizeof(item));
    memcpy(item.mac, mac, 6);
    item.type = BRIDGE_ITEM_FLUSH;
    xQueueSend(s_bridge_queue, &item, 0);
}

/* M7 Task 8: reports a bridge-routed ACTUATE's outcome (a DONE/FAILED ack
 * from the owning bridge node, or this hub's own TTL expiry) to the actor
 * layer through EXACTLY the path a locally dispatched Zigbee command
 * already uses -- zb_cmd.c's zb_cmd_report(), exported as
 * zb_cmd_report_public() for this purpose (zigbee.h). Same alert on
 * failure, same log line, same (here, always-NULL-registrant, hub-only)
 * s_result_cb hand-off, so an operator/the UI cannot tell a bridged
 * actuation's failure apart from a local one. A no-op for anything that
 * isn't an ACTUATE -- c->actor_dev_idx is -1 for PERMIT_JOIN/
 * DEVICE_REMOVE/DEVICE_RENAME/RESYNC (bridge_cmd_submit()'s callers above
 * only ever pass a real dev_idx for SWARM_CMD_ACTUATE), so there is no
 * actor command behind those to report against. */
static void report_bridge_result(const bridge_cmd_t *c, bool ok, uint8_t zcl_status, const char *reason)
{
    if (c->actor_dev_idx < 0) return;
    zb_cmd_report_public(c->actor_dev_idx, c->actor_action, c->actor_param, ok, reason, zcl_status);
}

static void bridge_task(void *arg)
{
    (void)arg;
    bridge_item_t it;
    for (;;) {
        BaseType_t got = xQueueReceive(s_bridge_queue, &it, pdMS_TO_TICKS(500));
        /* Sampled once per pass, whether or not an item arrived -- shared
         * by this pass's queue-item handling (if any) below AND the router
         * tick that runs every pass regardless (see this loop's tail). */
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

        if (got == pdTRUE) {
            /* M7 Task 7 fix round 1 (important #3): request_resync() itself
             * only enqueues (non-blocking, see its own comment) -- but the
             * ruling is that no "send" (however cheap) runs while
             * s_bridges_mutex is held, matching the router tick's and the
             * boot-time resync sweep's own "release, then send" discipline
             * just below/above. Set instead of calling directly inside the
             * switch; sent once, after xSemaphoreGive(), further down. Same
             * reasoning for flush_cmd (M7 zigbee bridge follow-up,
             * BRIDGE_ITEM_FLUSH below): bridge_cmd_peek_pending() itself is
             * a pure lookup (s_router needs no lock -- this task is its
             * only caller, see s_router's own top comment), but the
             * encode + espnow_link_send() that acts on it is exactly the
             * kind of "send" this discipline keeps out from under
             * s_bridges_mutex. */
            bool need_resync = false;
            const bridge_cmd_t *flush_cmd = NULL;
            bool do_cfg_flush = false;
            /* I2 fix: bridge_save() is no longer called per item -- ANNOUNCE/
             * GONE/STATUS-with-change below call mark_bridge_dirty(now_s)
             * instead (defined above, next to s_bridge_dirty), and this
             * loop's tail decides whether THIS pass is the one that actually
             * calls bridge_save(), after s_bridges_mutex is released. */

            xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
            switch (it.type) {
            case SWARM_MSG_DEVICE_ANNOUNCE: {
                /* M7 Task 7 fix round 1 (minor #4): bridge_table_upsert()
                 * can fail for two distinct reasons -- this node has never
                 * been seen before and every BRIDGE_MAX_NODES slot is
                 * already in use, or this node is already known but ITS
                 * device table (BRIDGE_MAX_DEVICES) is full. Checked
                 * BEFORE the upsert attempt (both still under
                 * s_bridges_mutex, so this can't race the upsert's own
                 * decision): if the node already existed, upsert() only
                 * ever consults bridge_table_node(mac, true) again -- which
                 * is guaranteed to find it, not fail -- so any upsert
                 * failure here can only be its device table being full;
                 * conversely, a brand-new node's device table starts at 0,
                 * always room for its very first device, so a failure for
                 * an unseen mac can only be the node table itself being full. */
                bool node_existed = bridge_table_node(&s_bridges, it.mac, false) != NULL;
                if (!bridge_table_upsert(&s_bridges, it.mac, &it.u.ann)) {
                    if (node_existed) {
                        ESP_LOGW(TAG, "bridge node " MACSTR ": device table full (%d), dropping announce",
                                 MAC2STR(it.mac), BRIDGE_MAX_DEVICES);
                    } else {
                        ESP_LOGW(TAG, "bridge node table full (%d), dropping announce from " MACSTR,
                                 BRIDGE_MAX_NODES, MAC2STR(it.mac));
                    }
                    break;
                }
                device_id_t id = { .kind = (device_kind_t)it.u.ann.dev.kind };
                memcpy(id.addr, it.u.ann.dev.addr, SWARM_ADDR_LEN);
                int idx = data_core_find_or_create_index(&id, now_s);
                if (idx < 0) break;   /* registry full: already counted/logged by data_core */
                data_core_set_via(idx, it.mac);
                /* param_max=0, no flags: every action a zigbee bridge announces
                 * today (On/Off, via zb_map.c on the bridge node's own side)
                 * takes no parameter -- same as zigbee.c's own actor_declare()
                 * call site (zb_register_restored_devices()). */
                for (uint8_t i = 0; i < it.u.ann.action_count; i++) {
                    if (!actor_declare(idx, it.u.ann.action_ids[i], 0, 0)) {
                        ESP_LOGW(TAG, "device %d: could not declare action %u from " MACSTR "'s announce",
                                 idx, (unsigned)it.u.ann.action_ids[i], MAC2STR(it.mac));
                    }
                }
                actor_set_device_key(idx, (const uint8_t *)&id);
                mark_bridge_dirty(now_s);
                break;
            }
            case SWARM_MSG_DEVICE_GONE: {
                bridge_table_remove(&s_bridges, it.mac, &it.u.gone.dev);
                mark_bridge_dirty(now_s);
                /* M7 Task 7 fix round 1 (critical #2): the registry never
                 * deletes rows (registry.h), so the device entry survives
                 * this device leaving its bridge's zigbee network -- but its
                 * via_node attribution is now stale (this bridge no longer
                 * reports it) and must not keep pointing at it. A later
                 * re-ANNOUNCE (this device rejoining, here or elsewhere)
                 * re-attributes via data_core_set_via() the same as any
                 * first-time announce does. */
                device_id_t id = { .kind = (device_kind_t)it.u.gone.dev.kind };
                memcpy(id.addr, it.u.gone.dev.addr, SWARM_ADDR_LEN);
                int idx = data_core_find_index(&id);
                if (idx >= 0) data_core_clear_via(idx);
                break;
            }
            case SWARM_MSG_MEASUREMENT: {
                device_id_t id = { .kind = (device_kind_t)it.u.meas.dev.kind };
                memcpy(id.addr, it.u.meas.dev.addr, SWARM_ADDR_LEN);
                if (data_core_find_index(&id) < 0) {
                    /* Unknown device: this bridge is reporting a reading for
                     * something it never announced (or this hub dropped/never
                     * saw the announce, e.g. it booted after the device already
                     * joined) -- ask it to resync rather than silently
                     * dropping forever. */
                    ESP_LOGW(TAG, "MEASUREMENT for unannounced device from " MACSTR ", requesting resync",
                             MAC2STR(it.mac));
                    need_resync = true;
                    break;
                }
                /* I4 fix: mirror the BLE relay path's honesty
                 * (data_core_submit_from()'s age policy) instead of always
                 * stamping this as "now". it.u.meas.age_s is the bridge
                 * node's own transmit-time age (already inclusive of any
                 * backlog wait, see forward_task()'s recompute); add on the
                 * time this item spent decoded-but-unprocessed (queued on
                 * s_bridge_queue since hub_rx_cb's item.recv_us) to get the
                 * TOTAL age as of right now. Clamped at UINT16_MAX the same
                 * way the node's own recompute is -- data_core_submit_cap_id_aged()
                 * drops anything past DATA_CORE_MAX_AGE_S (1800 s) long
                 * before that clamp could matter. */
                int64_t queued_us = esp_timer_get_time() - it.recv_us;
                uint32_t queued_s = queued_us > 0 ? (uint32_t)(queued_us / 1000000) : 0;
                uint32_t total_age_s = it.u.meas.age_s + queued_s;
                uint16_t age_for_data_core = (total_age_s > UINT16_MAX) ? UINT16_MAX : (uint16_t)total_age_s;
                /* data_core_submit_cap_id_aged() already logs its own
                 * reason (too old, out-of-range value, or registry full)
                 * on a false return -- nothing further to log here. */
                bool ok = data_core_submit_cap_id_aged(&id, it.u.meas.cap_id, it.u.meas.value, age_for_data_core);
                ESP_LOGI(TAG, "bridge: measurement from " MACSTR " cap %u value %.3f age %us -> %s",
                         MAC2STR(it.mac), it.u.meas.cap_id, (double)it.u.meas.value, (unsigned)age_for_data_core,
                         ok ? "accepted" : "rejected");
                if (ok) rules_notify_value_update();
                break;
            }
            case SWARM_MSG_COORD_STATUS: {
                bridge_node_t *b = bridge_table_node(&s_bridges, it.mac, true);
                if (!b) {
                    ESP_LOGW(TAG, "bridge node table full, dropping COORD_STATUS from " MACSTR,
                             MAC2STR(it.mac));
                    break;
                }
                swarm_note_node_radio(it.mac, it.u.status.radio_role);
                bool mismatch = b->status_valid && it.u.status.device_count != b->count;
                /* I2 fix: only dirty the table when this status actually
                 * differs from what's already persisted -- a bridge's
                 * periodic re-announce of an unchanged COORD_STATUS (the
                 * common steady-state case) must not force a flash rewrite
                 * every time it arrives. Field-by-field, not memcmp: the
                 * struct has a padding byte (after `channel`) that a
                 * field-wise decode never initialises, so a raw memcmp
                 * could report "changed" on padding garbage alone. */
                bool status_changed = !b->status_valid
                    || b->status.radio_role    != it.u.status.radio_role
                    || b->status.formed        != it.u.status.formed
                    || b->status.channel       != it.u.status.channel
                    || b->status.pan_id        != it.u.status.pan_id
                    || b->status.permit_s      != it.u.status.permit_s
                    || b->status.device_count  != it.u.status.device_count;
                b->status = it.u.status;
                b->status_valid = true;
                if (status_changed) mark_bridge_dirty(now_s);
                if (mismatch || !b->synced_once) {
                    need_resync = true;
                    b->synced_once = true;
                }
                break;
            }
            case SWARM_MSG_COMMAND_ACK: {
                bridge_cmd_t done;
                if (!bridge_cmd_on_ack(&s_router, it.mac, &it.u.ack, now_s, &done)) {
                    ESP_LOGD(TAG, "COMMAND_ACK from " MACSTR ": seq=%u op=%u status=%u detail=%u "
                                  "(still in flight, or stale/unknown seq)",
                             MAC2STR(it.mac), it.u.ack.seq, it.u.ack.op, it.u.ack.status, it.u.ack.detail);
                    break;
                }
                bool ok = (it.u.ack.status == SWARM_ACK_DONE);
                ESP_LOGI(TAG, "COMMAND_ACK from " MACSTR ": seq=%u op=%u status=%u detail=%u -- %s",
                         MAC2STR(it.mac), it.u.ack.seq, it.u.ack.op, it.u.ack.status, it.u.ack.detail,
                         ok ? "done" : "failed");
                report_bridge_result(&done, ok, it.u.ack.detail,
                                      ok ? NULL : "bridge node reported the command failed");
                break;
            }
            case BRIDGE_ITEM_SUBMIT: {
                /* ESP_ERR_NOT_FOUND (Task 9's contract) applies ONLY to a
                 * synchronous caller (want_result -- swarm_bridge_permit/
                 * remove/rename()): an operator asking this hub to command a
                 * mac it has never heard a device announce/COORD_STATUS from
                 * gets a clear "not a bridge" answer rather than a silently
                 * queued command to what might not even be a zigbee node.
                 * Fire-and-forget posts (request_resync(), swarm_zb_dispatch()'s
                 * ACTUATEs) skip this check entirely -- gating THEM on a
                 * bridge_table entry already existing would defeat the one
                 * case request_resync() exists for (MEASUREMENT from a node
                 * this hub has no announce/table entry for yet, asking it to
                 * resync so one gets created), and swarm_zb_dispatch() only
                 * ever submits a mac bridge_table_find_device() just returned
                 * anyway, so the check would be a no-op there besides. */
                esp_err_t res;
                if (it.u.submit.want_result && !bridge_table_node(&s_bridges, it.mac, false)) {
                    res = ESP_ERR_NOT_FOUND;
                } else if (bridge_cmd_submit(&s_router, it.mac, it.u.submit.op, &it.u.submit.dev,
                                              it.u.submit.arg, it.u.submit.name, it.u.submit.name_len,
                                              it.u.submit.ttl_s, now_s, it.u.submit.actor_dev_idx,
                                              it.u.submit.actor_action, it.u.submit.actor_param)) {
                    res = ESP_OK;
                } else {
                    res = ESP_ERR_INVALID_STATE;   /* one already in flight for this node */
                }

                if (it.u.submit.want_result) {
                    /* A synchronous swarm_bridge_permit/remove/rename() caller
                     * is waiting on s_submit_done -- see submit_and_wait(). */
                    s_submit_result = res;
                    xSemaphoreGive(s_submit_done);
                } else if (res != ESP_OK) {
                    /* Fire-and-forget posts: request_resync() (actor_dev_idx
                     * < 0, nothing to report) and swarm_zb_dispatch()'s ACTUATE
                     * posts (actor_dev_idx >= 0 -- the dispatching task already
                     * returned without knowing this outcome, so THIS is the
                     * only place left to report a "too many commands already
                     * outstanding for this node" failure back to the actor
                     * layer, same 0xfe "busy" detail swarm_zb_dispatch() itself
                     * uses when it can't even queue the request). */
                    ESP_LOGW(TAG, "BRIDGE_ITEM_SUBMIT to " MACSTR " (op=%u) refused: %s",
                             MAC2STR(it.mac), (unsigned)it.u.submit.op, esp_err_to_name(res));
                    if (it.u.submit.actor_dev_idx >= 0) {
                        zb_cmd_report_public(it.u.submit.actor_dev_idx, it.u.submit.actor_action,
                                             it.u.submit.actor_param, false,
                                             "bridge command already in flight for this node", 0xfe);
                    }
                }
                break;
            }
            case BRIDGE_ITEM_FLUSH: {
                /* M7 zigbee bridge follow-up: mac just transmitted to this
                 * hub (that's what queued this FLUSH -- see
                 * post_bridge_flush()'s callers in hub_rx_cb), which is
                 * precisely the brief window this bench's finding says
                 * mac's WiFi receive path is actually open. If this router
                 * has a pending, not-yet-accepted command for it, grab the
                 * pointer now (peek only; no mutation) -- sent further
                 * down, outside s_bridges_mutex, same as need_resync. */
                flush_cmd = bridge_cmd_peek_pending(&s_router, it.mac);
                do_cfg_flush = true;   /* also flush a pending NODE_CONFIG (below, outside the mutex) */
                break;
            }
            default:
                break;
            }
            xSemaphoreGive(s_bridges_mutex);

            /* need_resync (fix round 1) runs here, AFTER s_bridges_mutex is
             * released, never inside the switch above -- request_resync()
             * follows the same discipline the router tick and
             * swarm_start_main()'s own boot sweep already use for their own
             * sends. it.mac is a plain byte array copied into this local
             * item, so it stays valid regardless of anything the mutex was
             * protecting. bridge_save() itself is no longer called from
             * here -- see the dirty-flag coalescing below, which decides
             * once per pass (not once per item) whether this is the pass
             * that actually persists. */
            if (need_resync) request_resync(it.mac);

            /* flush_cmd (M7 zigbee bridge follow-up): send the pending
             * command NOW, regardless of this router's own retry timer --
             * same encode + espnow_link_send() shape as the router tick
             * below, deliberately duplicated rather than shared because the
             * router tick's loop also has to re-poll bridge_cmd_next_send()
             * for every other due command this same pass, which a flush (at
             * most one mac, one peek) has no need for. bridge_cmd_mark_sent()
             * updates sent_s/sends on a successful encode so the router
             * tick's own bridge_cmd_next_send(), called later this same
             * pass, does not immediately re-offer (and re-send) the exact
             * same command again. flush_cmd aliases s_router (see
             * bridge_cmd_peek_pending()'s own doc comment) and nothing else
             * calls a bridge_cmd_* function on s_router between the peek
             * above and this use, so it is still valid here. */
            if (flush_cmd) {
                swarm_command_t cmd = { .seq = flush_cmd->seq, .op = flush_cmd->op, .dev = flush_cmd->dev,
                                         .arg = flush_cmd->arg, .name_len = flush_cmd->name_len };
                memcpy(cmd.name, flush_cmd->name, flush_cmd->name_len);
                cmd.ttl_s = (uint16_t)(flush_cmd->deadline_s > now_s ? (flush_cmd->deadline_s - now_s) : 1);
                uint8_t buf[64];
                size_t n = swarm_encode_command(&cmd, buf, sizeof(buf));
                if (n == 0) {
                    ESP_LOGE(TAG, "flush command encode failed for " MACSTR " (op=%u seq=%u)",
                             MAC2STR(it.mac), (unsigned)flush_cmd->op, (unsigned)flush_cmd->seq);
                } else {
                    esp_err_t err = espnow_link_send(it.mac, buf, n);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "flush command -> " MACSTR " (op=%u seq=%u) send failed: %s",
                                 MAC2STR(it.mac), (unsigned)flush_cmd->op, (unsigned)flush_cmd->seq,
                                 esp_err_to_name(err));
                    } else {
                        ESP_LOGI(TAG, "flush command -> " MACSTR " (op=%u seq=%u, send #%u)",
                                 MAC2STR(it.mac), (unsigned)flush_cmd->op, (unsigned)flush_cmd->seq,
                                 (unsigned)flush_cmd->sends + 1);
                    }
                    bridge_cmd_mark_sent(&s_router, it.mac, now_s);
                }
            }

            /* NODE_CONFIG flush (bench finding, M7 gate 7, 2026-09-08): a
             * radio-role change reaches the node over the SAME cold-send path
             * as router commands, so it needs the SAME open-RX window. If this
             * node's desired radio role differs from what it last reported,
             * send a NODE_CONFIG here, in the window its own frame just opened,
             * instead of waiting for its next 5-minute CHECKIN. The node acks
             * DONE and reboots; the ack clears "pending" (record_reported_radio
             * on a seq match), so this stops firing after the first delivery.
             * swarm_send_node_config() takes only s_stats_mutex (already
             * released s_bridges_mutex above), so no lock-order issue. */
            if (do_cfg_flush) {
                radio_role_t desired = swarm_store_node_desired_radio(it.mac);
                uint8_t rep; bool have_rep = swarm_node_reported_radio(it.mac, &rep);
                bool pending = have_rep ? ((uint8_t)desired != rep)
                                        : ((uint8_t)desired != (uint8_t)RADIO_ROLE_BLE);
                if (pending) {
                    esp_err_t err = swarm_send_node_config(it.mac, desired);
                    ESP_LOGI(TAG, "NODE_CONFIG flush -> " MACSTR " radio_role=%s: %s",
                             MAC2STR(it.mac), radio_role_str(desired), esp_err_to_name(err));
                }
            }
        }   /* if (got == pdTRUE) */

        /* I2 fix (save storms): coalesce bridge_save() calls. Rather than a
         * flash tmp+rename per ANNOUNCE/GONE/STATUS-with-change (which a
         * RESYNC replay burst -- up to ZB_STORE_MAX_DEVICES frames back to
         * back -- turned into a rewrite storm through the queue), persist
         * at most once per pass, and only when: the queue just went quiet
         * (got != pdTRUE, i.e. this pass's xQueueReceive() above timed out
         * rather than finding an item -- the table has stopped changing for
         * now) OR the dirty flag has stood for >= 2 s (so a continuous
         * burst still gets flushed periodically instead of waiting for it
         * to fully drain). bridge_save()'s own CRC32 check (s_bridge_saved_crc)
         * additionally skips the actual write on top of this when the
         * image turns out byte-identical to what's already on flash. */
        bool bridge_queue_drained = (got != pdTRUE);
        bool do_bridge_save = false;
        xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
        if (s_bridge_dirty && (bridge_queue_drained || (now_s - s_bridge_dirty_since_s) >= 2)) {
            s_bridge_dirty = false;
            do_bridge_save = true;
        }
        xSemaphoreGive(s_bridges_mutex);
        if (do_bridge_save) bridge_save();

        /* I6 fix: a hub whose radio role is NOT BLE never starts
         * ble_collector_start() (main.c's want_ble == false), so nothing
         * else on a hub ever pumps actor_service() -- without this, a
         * bridged (or local Zigbee) ACTUATE sits in the actor queue forever
         * and never reaches swarm_zb_dispatch(). Gated on radio_role_get()
         * != RADIO_ROLE_BLE so a BLE-role hub, where adv_decoder_task's own
         * loop already pumps this (ble_collector.c, under s_actors_wired),
         * is never double-pumped -- calling actor_service() twice from two
         * tasks would just be redundant work, not unsafe (actor_lock()
         * serialises it), but there is no reason to pay for it. Driven off
         * this task's own 500 ms tick, same cadence as everything else in
         * this loop's tail. */
        if (radio_role_get() != RADIO_ROLE_BLE) actor_service();

        /* Router tick (Task 8), driven every pass regardless of whether an
         * item arrived this time -- the 500 ms queue-receive timeout above
         * IS this task's periodic tick, so a quiet queue still retries a
         * due command or expires an overdue one on roughly that cadence.
         * s_router needs no lock: this task is its only caller (see
         * s_router's own top comment). Encoding + espnow_link_send()
         * deliberately run OUTSIDE s_bridges_mutex (already released just
         * above) -- same "blocking send never held under a lock" discipline
         * swarm_start_main()'s own post-boot resync sweep uses. */
        const bridge_cmd_t *c;
        while ((c = bridge_cmd_next_send(&s_router, now_s)) != NULL) {
            swarm_command_t cmd = { .seq = c->seq, .op = c->op, .dev = c->dev, .arg = c->arg,
                                     .name_len = c->name_len };
            memcpy(cmd.name, c->name, c->name_len);
            /* c->deadline_s was fixed at submit time (now + the caller's
             * ttl_s); re-derive the REMAINING seconds for the wire so a
             * retried send still tells the node an honestly-shrunk window,
             * rather than replaying the original ttl_s as if no time had
             * passed. Floored at 1: swarm_command_t.ttl_s == 0 would read,
             * on the node side, as "deadline already passed" (command_task()
             * derives actor_now_s() + ttl_s for ACTUATE) -- a command this
             * router still considers live must never be sent as already-dead. */
            cmd.ttl_s = (uint16_t)(c->deadline_s > now_s ? (c->deadline_s - now_s) : 1);
            uint8_t buf[64];
            size_t n = swarm_encode_command(&cmd, buf, sizeof(buf));
            if (n == 0) {
                ESP_LOGE(TAG, "bridge command encode failed for " MACSTR " (op=%u seq=%u)",
                         MAC2STR(c->mac), (unsigned)c->op, (unsigned)c->seq);
                continue;
            }
            esp_err_t err = espnow_link_send(c->mac, buf, n);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "command -> " MACSTR " (op=%u seq=%u) send failed: %s",
                         MAC2STR(c->mac), (unsigned)c->op, (unsigned)c->seq, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "command -> " MACSTR " (op=%u seq=%u, send #%u)",
                         MAC2STR(c->mac), (unsigned)c->op, (unsigned)c->seq, (unsigned)c->sends);
            }
        }

        bridge_cmd_t expired;
        while (bridge_cmd_expire(&s_router, now_s, &expired)) {
            ESP_LOGW(TAG, "command -> " MACSTR " (op=%u seq=%u) timed out with no DONE/FAILED ack",
                     MAC2STR(expired.mac), (unsigned)expired.op, (unsigned)expired.seq);
            report_bridge_result(&expired, false, 0xff, "bridge command timed out");
        }
    }
}

/* Idempotent; safe to call more than once. Must run before any bridge
 * frame can be queued -- swarm_start_main() calls this right after
 * ensure_checkin_task(), same eager-init reasoning as that function's own
 * comment. */
static esp_err_t ensure_bridge_task(void)
{
    if (s_bridge_task) return ESP_OK;
    if (!s_bridges_mutex) s_bridges_mutex = xSemaphoreCreateMutex();
    if (!s_bridges_mutex) return ESP_ERR_NO_MEM;
    if (!s_bridge_queue) s_bridge_queue = xQueueCreate(BRIDGE_QUEUE_LEN, sizeof(bridge_item_t));
    if (!s_bridge_queue) return ESP_ERR_NO_MEM;
    /* M7 Task 8: submit_and_wait()'s synchronous swarm_bridge_permit/
     * remove/rename() answer path -- see s_submit_done's own top comment. */
    if (!s_submit_done) s_submit_done = xSemaphoreCreateBinary();
    if (!s_submit_done) return ESP_ERR_NO_MEM;
    if (!s_submit_call_mutex) s_submit_call_mutex = xSemaphoreCreateMutex();
    if (!s_submit_call_mutex) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(bridge_task, "swarm_bridge", 4096, NULL, 3, &s_bridge_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ensure_bridge_task: xTaskCreate failed");
        s_bridge_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* M7 Task 8: default TTL for a hub-initiated PERMIT_JOIN/DEVICE_REMOVE/
 * DEVICE_RENAME (swarm_bridge_permit/remove/rename() below) -- originally
 * 30 s (the same value request_resync() still uses for RESYNC, generous
 * over one ESP-NOW round trip plus this router's own one retry,
 * BRIDGE_CMD_RETRY_S). Raised to 60 s (M7 zigbee bridge follow-up): the
 * bench finding behind SWARM_MSG_POLL/BRIDGE_ITEM_FLUSH is that a
 * zigbee-role bridge's WiFi receive path is only reliably open right after
 * its OWN transmit, so even with the POLL-triggered early-send path this
 * is still a synchronous HTTP-handler-facing TTL and deserves more margin
 * than the fire-and-forget RESYNC path it was originally copied from. */
#define BRIDGE_API_TTL_S 60

/* Posts a BRIDGE_ITEM_SUBMIT for `mac` and blocks (this caller's own task,
 * never bridge_task or the ESP-NOW receive callback -- see swarm.h's
 * swarm_bridge_permit/remove/rename() doc comments, Task 9's HTTP handler
 * calls these directly and can afford to wait) up to 300 ms for
 * bridge_task to answer through s_submit_done/s_submit_result.
 * s_submit_call_mutex serialises concurrent callers so one caller's answer
 * can never be mistaken for another's (see s_submit_done's own top
 * comment). Returns ESP_ERR_INVALID_STATE when this hub has no bridge
 * task/queue/semaphores at all (ensure_bridge_task() never ran or failed),
 * when the queue was momentarily full, or when bridge_task never answered
 * within 300 ms (a wedged/overloaded bridge_task, or -- in practice -- a
 * test double with no bridge_task running at all); otherwise the result
 * bridge_task itself computed (ESP_OK, ESP_ERR_NOT_FOUND, or
 * ESP_ERR_INVALID_STATE for "one already in flight"). */
static esp_err_t submit_and_wait(const uint8_t mac[6], uint8_t op, const swarm_dev_addr_t *dev,
                                 uint16_t arg, const char *name, uint8_t name_len)
{
    if (!s_bridge_queue || !s_submit_done || !s_submit_call_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_submit_call_mutex, portMAX_DELAY);

    bridge_item_t item;
    memset(&item, 0, sizeof(item));
    memcpy(item.mac, mac, 6);
    item.type = BRIDGE_ITEM_SUBMIT;
    item.u.submit.op = op;
    if (dev) item.u.submit.dev = *dev;
    item.u.submit.arg = arg;
    if (name && name_len > 0) {
        uint8_t n = name_len > SWARM_DEV_NAME_MAX ? SWARM_DEV_NAME_MAX : name_len;
        memcpy(item.u.submit.name, name, n);
        item.u.submit.name_len = n;
    }
    item.u.submit.ttl_s = BRIDGE_API_TTL_S;
    item.u.submit.actor_dev_idx = -1;
    item.u.submit.want_result = true;

    esp_err_t result;
    if (xQueueSend(s_bridge_queue, &item, 0) != pdTRUE) {
        result = ESP_ERR_INVALID_STATE;
    } else if (xSemaphoreTake(s_submit_done, pdMS_TO_TICKS(300)) == pdTRUE) {
        result = s_submit_result;
    } else {
        ESP_LOGW(TAG, "submit_and_wait: bridge_task did not answer within 300ms for " MACSTR
                      " (op=%u)", MAC2STR(mac), (unsigned)op);
        result = ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(s_submit_call_mutex);
    return result;
}

/* See swarm.h. */
esp_err_t swarm_bridge_permit(const uint8_t mac[6])
{
    return submit_and_wait(mac, SWARM_CMD_PERMIT_JOIN, NULL, 0, NULL, 0);
}

/* See swarm.h. dev.kind is set for consistency with every other
 * swarm_dev_addr_t this component builds (ANNOUNCE/ACTUATE alike all use
 * DEV_KIND_ZIGBEE) even though the bridge node's own command_task()
 * matches DEVICE_REMOVE/DEVICE_RENAME purely on the 8-byte eui64
 * (memcmp(list[i].eui64, c.dev.addr, 8)) and never reads dev.kind for
 * these two ops -- there is no OTHER kind a zigbee bridge's own joined
 * devices could be, so leaving it unset would only be a false economy. */
esp_err_t swarm_bridge_remove(const uint8_t mac[6], const uint8_t eui64[8])
{
    swarm_dev_addr_t dev = { .kind = DEV_KIND_ZIGBEE };
    memcpy(dev.addr, eui64, SWARM_ADDR_LEN);
    return submit_and_wait(mac, SWARM_CMD_DEVICE_REMOVE, &dev, 0, NULL, 0);
}

/* See swarm.h and swarm_bridge_remove()'s comment above on dev.kind. */
esp_err_t swarm_bridge_rename(const uint8_t mac[6], const uint8_t eui64[8], const char *name)
{
    swarm_dev_addr_t dev = { .kind = DEV_KIND_ZIGBEE };
    memcpy(dev.addr, eui64, SWARM_ADDR_LEN);
    size_t len = name ? strlen(name) : 0;
    return submit_and_wait(mac, SWARM_CMD_DEVICE_RENAME, &dev, 0, name,
                           (uint8_t)(len > SWARM_DEV_NAME_MAX ? SWARM_DEV_NAME_MAX : len));
}

/* See swarm.h. A plain memberwise copy under s_bridges_mutex -- bounded and
 * cheap (BRIDGE_MAX_NODES * sizeof(bridge_node_t), the same table
 * bridge_save()/bridge_load() already move through s_bridge_io_buf a whole
 * serialised copy of), so unlike submit_and_wait() this never queues or
 * waits on bridge_task; it just takes the mutex bridge_task itself holds
 * only for the short, bounded span of one queue item's processing. */
bool swarm_bridge_snapshot(bridge_table_t *out)
{
    if (!s_bridges_mutex) return false;
    xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
    *out = s_bridges;
    xSemaphoreGive(s_bridges_mutex);
    return true;
}

static void hub_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    int type = swarm_frame_type(data, (size_t)len);
    if (type == -1 && len >= 1 && data[0] == 3) {
        /* M7 Task 7 (spec section 5.4): a node still running protocol v3
         * (pre-M7) sends frames whose version byte is 3, which
         * swarm_frame_type()'s version check above already rejects as -1 --
         * indistinguishable, from that return value alone, from any other
         * malformed/foreign frame. Give the operator an ACTIONABLE log
         * instead of a silent, unexplained drop. Throttled per-sender (not
         * globally), same bounded/allocation-free shape as
         * data_core.c's s_unreg_warned: a persistently un-reflashed node
         * shouldn't get lost among other WARNs, but also shouldn't flood
         * the log on every one of its retransmits. */
        static struct { uint8_t mac[6]; int64_t last_us; bool used; } s_v3_warn[4];
        static uint8_t s_v3_warn_next;
        int64_t now_us = esp_timer_get_time();
        int slot = -1;
        for (size_t i = 0; i < sizeof(s_v3_warn) / sizeof(s_v3_warn[0]); i++) {
            if (s_v3_warn[i].used && memcmp(s_v3_warn[i].mac, src_mac, 6) == 0) { slot = (int)i; break; }
        }
        if (slot < 0) {
            for (size_t i = 0; i < sizeof(s_v3_warn) / sizeof(s_v3_warn[0]); i++) {
                if (!s_v3_warn[i].used) { slot = (int)i; break; }
            }
            if (slot < 0) {
                slot = s_v3_warn_next;
                s_v3_warn_next = (uint8_t)((s_v3_warn_next + 1) % (sizeof(s_v3_warn) / sizeof(s_v3_warn[0])));
            }
            memcpy(s_v3_warn[slot].mac, src_mac, 6);
            s_v3_warn[slot].used = true;
            s_v3_warn[slot].last_us = 0;
        }
        if (now_us - s_v3_warn[slot].last_us > 60000000) {
            ESP_LOGW(TAG, "v3 frame from " MACSTR " dropped; reflash that node (protocol v4)",
                     MAC2STR(src_mac));
            s_v3_warn[slot].last_us = now_us;
        }
        return;
    }
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
        item.config_only = false;
        item.checkin = c;
        if (!s_checkin_queue || xQueueSend(s_checkin_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "CHECKIN from " MACSTR ": no checkin task available or queue full, dropping",
                     MAC2STR(src_mac));
        }
        /* M7 zigbee bridge follow-up: this node just transmitted to us --
         * see post_bridge_flush()'s own comment for why that's the trigger
         * to try pushing any pending command through right now. */
        post_bridge_flush(src_mac);
        return;
    }
    if (type == SWARM_MSG_POLL) {
        /* Node -> hub, header-only liveness poll (M7 zigbee bridge
         * follow-up) -- same pairing/spoofing reasoning as CHECKIN/READING
         * above: is_paired_node() is the gate that keeps an unpaired
         * device from injecting one. No ack, no fields beyond the frame
         * type itself, and nothing to decode -- its only purpose is this
         * node having just transmitted, which post_bridge_flush() below
         * uses as the trigger to try pushing a pending command through
         * right now (see swarm_frame.h's SWARM_MSG_POLL and swarm.c's
         * poll_task, node side, for the bench finding behind this). */
        if (!is_paired_node(src_mac)) return;
        record_stat(src_mac, rssi);
        post_bridge_flush(src_mac);
        return;
    }
    if (type == SWARM_MSG_NODE_CONFIG_ACK) {
        /* Node -> hub, unicast, encrypted (M7 Task 4) -- same pairing/
         * spoofing reasoning as CHECKIN above: is_paired_node() is the gate
         * that keeps an unpaired device from injecting a fake ack.
         *
         * Fix round 1: a DONE ack (the node has ALREADY applied the role --
         * see node_config_task()'s own comment for why ACCEPTED, meaning
         * only "queued", was the wrong status here) whose seq matches the
         * last cfg_seq THIS hub sent to THIS node is credited immediately
         * via record_reported_radio(), using the node's current desired
         * radio role (what this NODE_CONFIG asked it to become) -- so
         * radio_role_pending clears the moment the hub hears back, instead
         * of waiting for this node's next PAIR_REQ/COORD_STATUS. A seq
         * mismatch (a DONE for a stale/superseded request -- e.g. this hub
         * restarted and re-sent with a fresh cfg_seq before an old ack
         * arrived) or a FAILED status changes nothing here: the hub's next
         * reconciliation opportunity (this node's next CHECKIN, or a fresh
         * swarm_request_node_config()) recomputes desired vs reported from
         * scratch regardless, same self-healing shape as CHECKIN_ACK/
         * NODE_CONFIG sends above. */
        if (!is_paired_node(src_mac)) return;
        swarm_node_config_ack_t ack;
        if (!swarm_decode_node_config_ack(data, (size_t)len, &ack)) return;
        if (ack.status == SWARM_ACK_DONE) {
            bool seq_match = false;
            if (s_stats_mutex) {
                xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
                for (int i = 0; i < SWARM_MAX_NODES; i++) {
                    if (s_stats[i].in_use && memcmp(s_stats[i].mac, src_mac, 6) == 0) {
                        seq_match = (s_stats[i].cfg_seq == ack.seq);
                        break;
                    }
                }
                xSemaphoreGive(s_stats_mutex);
            }
            if (seq_match) {
                radio_role_t desired = swarm_store_node_desired_radio(src_mac);
                record_reported_radio(src_mac, (uint8_t)desired);
                ESP_LOGI(TAG, "NODE_CONFIG_ACK from " MACSTR ": done (seq=%u), radio_role=%s confirmed",
                         MAC2STR(src_mac), ack.seq, radio_role_str(desired));
            } else {
                ESP_LOGW(TAG, "NODE_CONFIG_ACK from " MACSTR ": done but seq=%u doesn't match the "
                              "last request sent to it -- ignoring (stale/superseded)",
                         MAC2STR(src_mac), ack.seq);
            }
        } else {
            ESP_LOGW(TAG, "NODE_CONFIG_ACK from " MACSTR ": refused (seq=%u status=%u)",
                     MAC2STR(src_mac), ack.seq, ack.status);
        }
        /* M7 zigbee bridge follow-up: see post_bridge_flush()'s own comment. */
        post_bridge_flush(src_mac);
        return;
    }
    if (type == SWARM_MSG_DEVICE_ANNOUNCE || type == SWARM_MSG_DEVICE_GONE ||
        type == SWARM_MSG_MEASUREMENT || type == SWARM_MSG_COORD_STATUS ||
        type == SWARM_MSG_COMMAND_ACK) {
        /* Bridge -> hub, unicast, encrypted (M7 Task 7) -- same pairing/
         * spoofing reasoning as READING/CHECKIN above: is_paired_node() is
         * the gate that keeps an unpaired device from injecting a fake
         * device announce/measurement/status straight into the registry.
         * Decoding happens HERE, not in bridge_task(), so a malformed frame
         * from a paired-but-buggy sender never reaches the queue at all --
         * only a successfully decoded item is queued. This callback only
         * ever queues, never touches s_bridges or data_core directly: both
         * data_core_find_or_create_index() and espnow_link_send()
         * (bridge_task's request_resync()) can take a lock/block, neither
         * of which belongs on the ESP-NOW receive callback (WiFi driver
         * task) -- same "callback queues, task sends/writes" shape as
         * CHECKIN above. */
        if (!is_paired_node(src_mac)) return;
        bridge_item_t item;
        memset(&item, 0, sizeof(item));
        memcpy(item.mac, src_mac, 6);
        item.type = type;
        item.recv_us = esp_timer_get_time();   /* I4 fix: see bridge_item_t's own comment */
        bool ok;
        switch (type) {
        case SWARM_MSG_DEVICE_ANNOUNCE: ok = swarm_decode_device_announce(data, (size_t)len, &item.u.ann); break;
        case SWARM_MSG_DEVICE_GONE:     ok = swarm_decode_device_gone(data, (size_t)len, &item.u.gone); break;
        case SWARM_MSG_MEASUREMENT:     ok = swarm_decode_measurement(data, (size_t)len, &item.u.meas); break;
        case SWARM_MSG_COORD_STATUS:    ok = swarm_decode_coord_status(data, (size_t)len, &item.u.status); break;
        case SWARM_MSG_COMMAND_ACK:     ok = swarm_decode_command_ack(data, (size_t)len, &item.u.ack); break;
        default:                        ok = false; break;   /* unreachable: the outer if() already narrowed type */
        }
        if (!ok) return;
        /* I3 fix: same record_stat() call the READING/CHECKIN branches
         * above already make, at the same point (after a successful
         * decode, before queuing) -- without it, a bridge streaming
         * MEASUREMENT/COORD_STATUS/etc. every few seconds still only moves
         * last_seen_s/frames_rx/rssi on its ~300 s CHECKIN cadence, making
         * an actively-talking bridge look nearly lost, and (via
         * swarm_note_node_radio()'s "existing slot only" contract) drops
         * every COORD_STATUS radio-role report until that first CHECKIN
         * creates the stats slot. */
        record_stat(src_mac, rssi);
        if (!s_bridge_queue || xQueueSend(s_bridge_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "bridge frame (type=%d) from " MACSTR ": no bridge task/queue full, dropping",
                     type, MAC2STR(src_mac));
        }
        /* M7 zigbee bridge follow-up: see post_bridge_flush()'s own comment. */
        post_bridge_flush(src_mac);
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

/* cmd->deadline_s (actor_cmd_t, actor.h) is absolute, actor_now_s()-scale
 * (== esp_timer_get_time()/1e6, the same clock this file's now_s already
 * uses throughout) -- bridge_cmd_submit() wants a duration instead, so
 * this re-derives one. Floored at 1 for the same reason bridge_task's own
 * retry-send re-derivation is (a command already past its actor-side
 * deadline must still get SOME positive ttl_s here, rather than 0 reading
 * as "already expired" once it reaches the node -- and this router's own
 * bridge_cmd_expire() is the backstop that actually cuts off a truly
 * overdue command, not this clamp). */
static uint32_t deadline_to_ttl(uint32_t deadline_s)
{
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    return deadline_s > now_s ? (deadline_s - now_s) : 1;
}

/* M7 Task 8: the hub's actor dispatch hook for DEV_KIND_ZIGBEE, registered
 * below (swarm_start_main()) INSTEAD OF zb_cmd_start()'s own direct
 * zb_cmd_local_dispatch() registration -- see zb_cmd_set_router_active()'s
 * doc comment (zigbee.h) for the boot-order reasoning behind that swap.
 *
 * Looks the dispatched device up in the bridge table: one attributed to a
 * bridge NODE's zigbee coordinator is routed there as a SWARM_CMD_ACTUATE,
 * through the exact same BRIDGE_ITEM_SUBMIT/bridge_task path every other
 * hub-initiated command already uses -- one place sends commands, per this
 * task's brief. A device with no bridge attribution (overwhelmingly: it's
 * on the HUB's OWN zigbee coordinator; also covers "no bridge table at
 * all", e.g. ensure_bridge_task() never started) falls straight through to
 * zb_cmd_local_dispatch() -- exactly the dispatch a hub with no router
 * would have registered directly, so a non-bridged device's behaviour is
 * byte-for-byte unchanged by this wrapper's existence.
 *
 * Never blocks actor_service()'s caller (ble_collector.c's
 * adv_decoder_task, on the hub): the bridge-table lookup takes
 * s_bridges_mutex only for a short, bounded scan (bridge_task, this
 * mutex's only other holder, never sleeps while holding it either -- see
 * s_bridges_mutex's own top comment), and the actual submit is a single
 * non-blocking xQueueSend(). A queue that's momentarily full reports the
 * failure itself, right here (0xfe "busy", same code swarm_zb_dispatch's
 * own bridge_task-side "already in flight" case uses) -- there is no other
 * place left to report it, since bridge_task never even saw this item. */
static void swarm_zb_dispatch(const actor_cmd_t *cmd)
{
    uint8_t key[ACTOR_DEVICE_KEY_LEN];
    if (!actor_device_key(cmd->dev_idx, key)) {
        zb_cmd_local_dispatch(cmd);
        return;
    }
    swarm_dev_addr_t dev = { .kind = key[0] };
    memcpy(dev.addr, key + 1, SWARM_ADDR_LEN);

    uint8_t owner_mac[6];
    bool routed = false;
    if (s_bridges_mutex) {
        xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
        const bridge_node_t *b = bridge_table_find_device(&s_bridges, &dev);
        if (b) {
            memcpy(owner_mac, b->mac, 6);
            routed = true;
        }
        xSemaphoreGive(s_bridges_mutex);
    }
    if (!routed) {
        zb_cmd_local_dispatch(cmd);
        return;
    }

    bridge_item_t it;
    memset(&it, 0, sizeof(it));
    memcpy(it.mac, owner_mac, 6);
    it.type = BRIDGE_ITEM_SUBMIT;
    it.u.submit.op = SWARM_CMD_ACTUATE;
    it.u.submit.dev = dev;
    it.u.submit.arg = (uint16_t)((uint16_t)cmd->action_id | ((uint16_t)cmd->param << 8));
    it.u.submit.ttl_s = deadline_to_ttl(cmd->deadline_s);
    it.u.submit.actor_dev_idx = cmd->dev_idx;
    it.u.submit.actor_action = cmd->action_id;
    it.u.submit.actor_param = cmd->param;
    it.u.submit.want_result = false;

    if (!s_bridge_queue || xQueueSend(s_bridge_queue, &it, 0) != pdTRUE) {
        zb_cmd_report_public(cmd->dev_idx, cmd->action_id, cmd->param, false,
                             "bridge command queue full", 0xfe);
    }
}

esp_err_t swarm_start_main(void)
{
    if (!s_stats_mutex) s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) return ESP_ERR_NO_MEM;

    /* Created here, BEFORE bridge_load() below (which takes it) -- unlike
     * s_bridges_mutex, which ensure_bridge_task() creates lazily further
     * down, this one has a user before that point. See s_bridge_io_buf's
     * own top comment for the lock-order/why. */
    if (!s_bridge_io_mutex) s_bridge_io_mutex = xSemaphoreCreateMutex();
    if (!s_bridge_io_mutex) return ESP_ERR_NO_MEM;

    /* I6 fix: unconditional, not gated on radio role -- a hub in the
     * zigbee role (no ble_collector_start() call at all, main.c's
     * want_ble == false) previously never got a live actor table: raw BSS
     * meant every row's dev_idx read 0 instead of the -1 free sentinel, so
     * bridge_task's actor_declare()/actor_set_device_key() calls further
     * down (its DEVICE_ANNOUNCE case) mis-attributed against an
     * uninitialised table. actor_init() is idempotent (its own doc
     * comment), so a BLE-role hub's later ble_collector_start() ->
     * actor_init() call is a harmless no-op, not a second wipe. */
    actor_init();

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

    /* M7 Task 7: restores the bridge table BEFORE ensure_bridge_task() below
     * can start bridge_task -- same "load before anything else can touch
     * it" ordering as zigbee.c's zb_store_load(), and for the same reason:
     * bridge_load() itself takes no lock (nothing else exists yet to race
     * it against). */
    bridge_load();

    /* M7 Task 8: claims the DEV_KIND_ZIGBEE actor dispatch hook for
     * swarm_zb_dispatch() (this file's own wrapper, defined above) BEFORE
     * zigbee_start()'s later zb_cmd_start() call (main.c calls
     * swarm_start_main() well ahead of zigbee_start() -- see
     * zb_cmd_set_router_active()'s own doc comment in zigbee.h for the
     * full boot-order reasoning). Sets the flag first, then registers the
     * hook -- either order is race-free here (both run on this same boot
     * task, and zb_cmd_start() cannot run until zigbee_start() does, later
     * still), but setting the flag first matches its own name: by the time
     * anything could observe the hook, the flag already explains why. Runs
     * unconditionally -- even if ensure_bridge_task() below fails,
     * swarm_zb_dispatch() degrades gracefully to zb_cmd_local_dispatch()
     * for every device (see its own top comment), which is no worse than
     * what a router-less hub would have registered directly. */
    zb_cmd_set_router_active(true);
    actor_set_dispatch_hook(DEV_KIND_ZIGBEE, swarm_zb_dispatch);

    err = ensure_bridge_task();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ensure_bridge_task failed: %s -- bridge (zigbee) frames will go unanswered",
                 esp_err_to_name(err));
    } else if (s_bridges_mutex) {
        /* A zigbee bridge only re-announces its devices on its OWN boot or
         * RESYNC (Task 5/6); if THIS hub rebooted while a bridge stayed up,
         * that bridge has no reason to re-send anything unprompted. Ask
         * explicitly for every node this hub has a stored status for.
         * synced_once is RAM-only (bridge_node_t's own doc comment) and
         * starts false every boot regardless of what was persisted --
         * marking it true here avoids a redundant second RESYNC the moment
         * that node's first post-reboot COORD_STATUS arrives (bridge_task's
         * COORD_STATUS case: "mismatch || !b->synced_once"). Snapshot the
         * MAC list under the mutex, then request outside it: request_resync()
         * itself is a non-blocking queue post (M7 Task 8 -- it no longer
         * calls espnow_link_send() directly, bridge_task's router does that
         * later), but xQueueSend() still must not run while holding a mutex
         * bridge_task/swarm_forget_node_stats() might otherwise need -- same
         * "never call out while holding this lock" discipline as everywhere
         * else in this file. */
        uint8_t macs[BRIDGE_MAX_NODES][6];
        int resync_n = 0;
        xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
        for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
            if (s_bridges.n[i].in_use && s_bridges.n[i].status_valid) {
                memcpy(macs[resync_n++], s_bridges.n[i].mac, 6);
                s_bridges.n[i].synced_once = true;
            }
        }
        xSemaphoreGive(s_bridges_mutex);
        for (int i = 0; i < resync_n; i++) request_resync(macs[i]);
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
        /* M7 Task 4: "radio_role" is this node's DESIRED radio role
         * (swarm_store's per-node table, set by POST /api/v1/nodes/{MAC12}
         * {"radio_role":...}) -- same "operator intent, shown immediately"
         * reasoning as "power_mode" above. "reported_radio_role" is null
         * until this node's radio role has actually been learned this boot
         * (PAIR_REQ or COORD_STATUS -- see record_reported_radio()); unlike
         * reported_mode, there is no CHECKIN carrying this, so a node that
         * hasn't (re)paired or sent a COORD_STATUS this boot simply has
         * nothing to report yet, independent of whether it has ever
         * checked in. "radio_role_pending" mirrors power_mode_pending's own
         * shape, kept in sync with checkin_task()'s own radio_pending
         * computation just above -- see that comment for why "never
         * reported" defaults to comparing against RADIO_ROLE_BLE rather
         * than treating an unknown report as automatically compliant. */
        radio_role_t dr = swarm_store_node_desired_radio(mac);
        cJSON_AddStringToObject(o, "radio_role", radio_role_str(dr));
        if (stat && stat->reported_radio_valid)
            cJSON_AddStringToObject(o, "reported_radio_role", radio_role_str((radio_role_t)stat->reported_radio_role));
        else
            cJSON_AddNullToObject(o, "reported_radio_role");
        bool rpending = stat && stat->reported_radio_valid ? (stat->reported_radio_role != (uint8_t)dr)
                                                             : (dr != RADIO_ROLE_BLE);
        cJSON_AddBoolToObject(o, "radio_role_pending", rpending);
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
 * same mutex.
 *
 * M7 Task 7: also forgets mac's bridge table entry (its coordinator status
 * and every device it announced) -- without this, a re-paired replacement
 * node reusing the same registry attribution would find a stale bridge
 * entry left behind by whichever node this MAC used to belong to. The
 * table mutation runs under s_bridges_mutex, the same lock bridge_task
 * uses, since this can run concurrently with it (unlike s_stats above,
 * which only bridge_task and this function ever touch, this table has two
 * writers). api_v1.c's forget handler already calls
 * data_core_clear_node_attribution(mac) itself right alongside this call --
 * not duplicated here.
 *
 * I2 fix: this no longer calls bridge_save() itself -- it marks the table
 * dirty (mark_bridge_dirty(), under s_bridges_mutex, same as every other
 * writer) and leaves the actual persist to bridge_task's own coalescing,
 * so a forget cannot force an extra flash write outside that discipline. */
void swarm_forget_node_stats(const uint8_t mac[6])
{
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        for (int i = 0; i < SWARM_MAX_NODES; i++) {
            if (s_stats[i].in_use && memcmp(s_stats[i].mac, mac, 6) == 0) {
                memset(&s_stats[i], 0, sizeof(s_stats[i]));
                break;
            }
        }
        xSemaphoreGive(s_stats_mutex);
    }

    if (s_bridges_mutex) {
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        xSemaphoreTake(s_bridges_mutex, portMAX_DELAY);
        bridge_table_forget_node(&s_bridges, mac);
        /* I2 fix: mark dirty instead of calling bridge_save() directly --
         * this runs on the forget HTTP handler's own task, a different task
         * from bridge_task, so it participates in the same dirty-flag
         * coalescing (mark_bridge_dirty() is guarded by s_bridges_mutex,
         * held here) rather than forcing an out-of-band flash write of its
         * own. bridge_task picks it up on its very next pass (queue-drained
         * or the 2 s cap, whichever is first). */
        mark_bridge_dirty(now_s);
        xSemaphoreGive(s_bridges_mutex);
    }
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
/* Bench finding (M7, 2026-09-08): min spacing between backlog retries on a
 * bridge node whose sends time out under coex -- see forward_task(). */
#define BACKLOG_RETRY_GAP_MS 3000

static QueueHandle_t s_fwd_queue;
static uint8_t       s_hub_mac[6];   /* set once in swarm_start_node(); MAC never
                                       * changes across a resync, only the channel does */

/* M7 zigbee bridge follow-up: esp_timer_get_time() at the moment this node
 * last sent ANYTHING to s_hub_mac -- updated by note_hub_tx() (below),
 * called from every one of this file's node->hub espnow_link_send() sites
 * (forward_task, send_checkin_and_wait_ack, node_config_task's
 * NODE_CONFIG_ACK, send_cmd_ack's COMMAND_ACK). poll_task (below) reads
 * this to skip sending a POLL when a real frame already went out recently
 * enough to have opened the same post-TX WiFi receive window a POLL exists
 * to manufacture -- see poll_task's own comment for the bench finding
 * behind this. Plain int64_t, no lock: every writer runs on its own
 * dedicated task and a torn read here (this is not atomic on every target)
 * can at worst make poll_task decide one poll early or late, never anything
 * unsafe. */
static int64_t s_last_hub_tx_us;

static void note_hub_tx(void)
{
    s_last_hub_tx_us = esp_timer_get_time();
}

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

/* ---------------- Node side: NODE_CONFIG apply (M7 Task 4) ----------------
 *
 * node_rx_cb (the WiFi driver task) must never touch NVS or block, same
 * project-wide rule as every other deferred-work path in this file -- and
 * applying a NODE_CONFIG needs radio_role_set() (an NVS write) plus, on
 * success, esp_restart() (this node's whole reason for switching radios is
 * that BLE/802.15.4 controllers cannot be re-inited live -- see
 * radio_role.h). So the callback only ever decodes and does a non-blocking
 * send onto this depth-1 queue; node_config_task() (below) does the actual
 * work. Depth 1 is enough: a node has at most one NODE_CONFIG outstanding
 * at a time in practice (the hub only ever has one reason to send one --
 * reconciling this node's desired vs reported radio role -- and a rejected
 * or accepted config always answers with a NODE_CONFIG_ACK before the hub
 * would plausibly send another), and any stale leftover from a config this
 * node already restarted for is moot the instant that restart happens.
 * Created eagerly, in swarm_start_node() below, before espnow_link_init()
 * hands node_rx_cb its first frame -- same eager-init reasoning as
 * s_checkin_ack_queue above. */
static QueueHandle_t s_node_cfg_queue;

/* ---------------- Node side: COMMAND hand-off (M7 Task 6) ----------------
 *
 * Same DEFERRAL pattern, and same reasoning, as s_node_cfg_queue just
 * above: acting on a SWARM_MSG_COMMAND (opening permit-join, sending a ZCL
 * command, a store rename/remove, a boot-replay-style resync) needs the
 * Zigbee stack lock and/or a flash write, none of which node_rx_cb (the
 * WiFi driver task) may ever do -- so it only decodes and does a
 * non-blocking send onto this queue; command_task() (below, in the "node
 * side: command task" section) does the actual work. Depth 4, not 1: a
 * hub could plausibly have more than one command in flight for this node
 * (e.g. an ACTUATE followed immediately by a RESYNC) and, unlike
 * NODE_CONFIG/CHECKIN_ACK, this node does not itself gate how many the hub
 * sends before an ack comes back -- matches swarm_command_t's own seq
 * space and this being the size the brief specifies. Created eagerly, in
 * swarm_start_node() below, only for a zigbee-role node (the hub never
 * sends a COMMAND to a BLE-role node), before espnow_link_init() hands
 * node_rx_cb its first frame -- same eager-init reasoning as
 * s_checkin_ack_queue above. */
static QueueHandle_t s_cmd_queue;

/* Hand-off in the OTHER direction: zb_cmd_report() (zb_cmd.c) reports an
 * ACTUATE's outcome from on_zb_result() below, which may run on the Zigbee
 * stack task (a Default Response or a timeout) rather than command_task()'s
 * own task -- see zb_cmd_result_t's doc comment in zigbee.h. Sending the
 * SWARM_MSG_COMMAND_ACK itself needs espnow_link_send(), which this file's
 * other cross-task hand-offs never call directly from a producer either,
 * so on_zb_result() only ever does a non-blocking send onto this queue;
 * command_task() drains it on the same loop that drains s_cmd_queue. Depth
 * 1 is enough: s_actuate_pending (below) is single-slot, so at most one
 * ACTUATE outcome is ever outstanding at a time. Created eagerly, alongside
 * s_cmd_queue above. */
static QueueHandle_t s_ack_queue;

/* Pops a NODE_CONFIG off s_node_cfg_queue and applies it. Fix round 1:
 * first compares the requested role against this node's CURRENT role
 * (radio_role_is_set() ? radio_role_get() : RADIO_ROLE_BLE, same "unknown
 * treated as the default" reasoning as everywhere else in this file) --
 * without this, a hub whose own reported_radio_role only ever refreshes at
 * PAIR_REQ/COORD_STATUS would keep re-sending the SAME already-applied
 * role on every checkin (checkin_task()'s radio_pending check comparing
 * against a stale "unknown"/mismatched report), and this node would reboot
 * every single time it heard one -- a reboot loop for no actual change. If
 * the requested role already matches: ack DONE (it genuinely IS done,
 * trivially), no persist, no reboot. Otherwise, validate the requested
 * role against this node's CURRENT power mode (swarm_rules_node_radio_ok(),
 * Task 3 -- the same compatibility rule the hub itself checks before ever
 * sending this) and persist it (radio_role_set()); on success, ack DONE
 * and restart into the new role (the BT/802.15.4 controllers cannot be
 * re-inited live -- radio_role.h -- so this is the only way the change
 * actually takes effect). A refusal (rules violation, or radio_role_set()
 * itself failing) acks FAILED and changes nothing -- no persist, no
 * reboot, loops back for the next item. The ack status is DONE, not
 * ACCEPTED (M7 Task 4's original choice): by the time this ack goes out
 * the change (or the no-op) has already actually happened, not merely been
 * queued, so DONE is the accurate status -- see swarm_frame.h's
 * SWARM_ACK_* enum. Either way, the hub logs the result but does not retry
 * on a lost ack itself (see hub_rx_cb's NODE_CONFIG_ACK branch); this
 * node's own next PAIR_REQ/checkin still reports whatever radio role it is
 * ACTUALLY running, so the hub's view self-heals either way. */
static void node_config_task(void *arg)
{
    (void)arg;
    swarm_node_config_t cfg;
    for (;;) {
        if (xQueueReceive(s_node_cfg_queue, &cfg, portMAX_DELAY) != pdTRUE) continue;

        radio_role_t requested = (radio_role_t)cfg.radio_role;
        radio_role_t current = radio_role_is_set() ? radio_role_get() : RADIO_ROLE_BLE;
        bool already_running = (requested == current);
        bool ok = already_running;
        const char *why = NULL;
        if (!already_running) {
            ok = swarm_rules_node_radio_ok(requested, swarm_store_power_mode(), &why)
                 && radio_role_set(requested) == ESP_OK;
        }

        swarm_node_config_ack_t ack = { .seq = cfg.seq, .status = ok ? SWARM_ACK_DONE : SWARM_ACK_FAILED };
        uint8_t buf[16];
        size_t n = swarm_encode_node_config_ack(&ack, buf, sizeof(buf));
        if (n) {
            esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
            note_hub_tx();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "NODE_CONFIG_ACK send failed (%s), dropped -- the hub's next "
                              "reconciliation retries", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "NODE_CONFIG_ACK: failed to encode");
        }
        if (!ok) {
            ESP_LOGW(TAG, "node config refused: %s", why ? why : "?");
            continue;
        }
        if (already_running) {
            ESP_LOGI(TAG, "radio role already %s; NODE_CONFIG is a no-op, not rebooting",
                     radio_role_str(current));
            continue;
        }
        ESP_LOGW(TAG, "radio role -> %s by the hub; rebooting", radio_role_str(requested));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

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

    if (type == SWARM_MSG_NODE_CONFIG) {
        /* Hub -> node, unicast, encrypted (M7 Task 4): same source-check
         * reasoning as CHECKIN_ACK/PONG just below -- only a NODE_CONFIG
         * from this node's OWN stored hub is ever queued, so a stray or
         * spoofed frame from anyone else in radio range cannot force a
         * radio-role switch/reboot. Decode failure is silently dropped,
         * same as every other decoder call on this path; see
         * s_node_cfg_queue's own comment for why the actual apply is
         * deferred to node_config_task(). */
        swarm_node_config_t cfg;
        if (swarm_decode_node_config(data, (size_t)len, &cfg)) {
            uint8_t hub_mac[6];
            if (swarm_store_hub(hub_mac, NULL, NULL) && memcmp(src_mac, hub_mac, 6) == 0) {
                if (s_node_cfg_queue) xQueueSend(s_node_cfg_queue, &cfg, 0);
            }
        }
        return;
    }

    if (type == SWARM_MSG_COMMAND) {
        /* Hub -> node, unicast (M7 Task 6): same source-check reasoning as
         * NODE_CONFIG just above -- only a COMMAND from this node's own
         * stored hub is ever queued, so a stray or spoofed frame from
         * anyone else in radio range cannot permit-join, actuate, remove,
         * rename or resync this bridge. Decode failure is silently
         * dropped, same as every other decoder call on this path. Actually
         * acting on it (permit-join, actor_request(), a store lookup, a
         * flash-touching rename/remove) needs the Zigbee stack lock and/or
         * NVS, so -- same DEFERRAL pattern as NODE_CONFIG -- this callback
         * only decodes and does a non-blocking send onto s_cmd_queue;
         * command_task() (below) does the actual work. s_cmd_queue is only
         * ever non-NULL on a zigbee-role node (created in
         * swarm_start_node()'s zigbee block), so this is a silent no-op on
         * a BLE-role node that somehow received one. */
        swarm_command_t cmd;
        if (swarm_decode_command(data, (size_t)len, &cmd)) {
            uint8_t hub_mac[6];
            if (swarm_store_hub(hub_mac, NULL, NULL) && memcmp(src_mac, hub_mac, 6) == 0) {
                if (s_cmd_queue && xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "command queue full, dropping seq %u op %u", cmd.seq, cmd.op);
                }
            }
        }
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
    /* data_core.c now posts a full device_id_t (kind + 8-byte addr), not a
     * bare mac -- M6b fix round 1. This node's own ESP-NOW forward path
     * below builds a swarm_reading_t, a fixed V1 wire shape keyed by a bare
     * 6-byte mac (swarm_frame.h, out of scope for this milestone to
     * change), so only a BLE-kind update can be forwarded through it at
     * all -- reconstructing a DEV_KIND_BLE id from a non-BLE device's
     * address (the old device_id_from_mac(DEV_KIND_BLE, mac) shape) would
     * instead risk matching some unrelated real BLE device that happens to
     * share the first six address bytes.
     *
     * M7 Task 5: a zigbee-role node DOES now have node-side presence to
     * forward -- its own coordinator runs locally (radio_role.h), so a
     * DEV_KIND_ZIGBEE update is this node's own registry gaining a fresh
     * reading from a device it just interviewed, not something relayed
     * from elsewhere. It has no bare-mac wire shape to fight (its address
     * is the device's own EUI-64, carried whole in swarm_measurement_t's
     * swarm_dev_addr_t), so it gets its own branch below rather than being
     * squeezed through the BLE-shaped swarm_reading_t path. */
    const device_id_t *dev_id = data;
    if (dev_id->kind == DEV_KIND_ZIGBEE) {
        device_entry_t d;
        if (!data_core_get_device(dev_id, &d)) return;
        /* One MEASUREMENT per valid capability whose value changed since we
         * last forwarded it: data_core posts one event per submit, so send
         * the freshest slot only -- the one with the newest timestamp. */
        int best = -1;
        for (int c = 0; c < CAPABILITY_COUNT; c++)
            if (d.caps[c].valid && (best < 0 || d.caps[c].updated_s >= d.caps[best].updated_s)) best = c;
        if (best < 0) return;
        swarm_out_t o = { .tag = SWARM_OUT_MEASUREMENT };
        o.u.meas.dev.kind = DEV_KIND_ZIGBEE;
        memcpy(o.u.meas.dev.addr, dev_id->addr, SWARM_ADDR_LEN);
        o.u.meas.cap_id = (uint8_t)best;
        o.u.meas.value = capability_decode((uint8_t)best, d.caps[best].raw);
        o.u.meas.age_s = 0;
        if (!s_fwd_queue || xQueueSend(s_fwd_queue, &o, 0) != pdTRUE)
            ESP_LOGW(TAG, "forward queue full, dropping measurement");
        return;
    }
    if (dev_id->kind != DEV_KIND_BLE) return;
    const uint8_t *mac = dev_id->addr;
    /* Single-device lookup (data_core_get_device(), ~124 B out-param) rather
     * than a full registry_t snapshot: this handler runs on the default
     * event-loop task, which this file's own comments document as having
     * only ~2304 bytes of stack -- see data_core.h's doc comment on
     * data_core_get_device() for the same reasoning applied there. */
    device_entry_t d;
    if (!data_core_get_device(dev_id, &d)) return;

    /* Decode each capability slot back to its V1 swarm_reading_t field
     * shape (deci-C, %, lux, uS/cm, %) -- same conversions the M2 registry-
     * compat shim used to do (data_core.c's snapshot_legacy_one(), deleted
     * Task 7); replicated locally here since swarm_reading_t's wire format
     * (this file's swarm_frame.h) is fixed V1-shape and out of scope for
     * this milestone to change. */
    swarm_out_t o = { .tag = SWARM_OUT_READING };
    swarm_reading_t *r = &o.u.reading;
    *r = (swarm_reading_t){
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_READING,
        .frame_cnt = d.last_frame_cnt,
        .temp_dc = d.caps[CAP_AIR_TEMPERATURE].valid
            ? (int16_t)lroundf(capability_decode(CAP_AIR_TEMPERATURE, d.caps[CAP_AIR_TEMPERATURE].raw) * 10.0f)
            : INT16_MIN,
        .moisture_pct = d.caps[CAP_SOIL_MOISTURE].valid
            ? (uint8_t)lroundf(capability_decode(CAP_SOIL_MOISTURE, d.caps[CAP_SOIL_MOISTURE].raw))
            : 0xFF,
        .battery_pct = d.caps[CAP_BATTERY_LEVEL].valid
            ? (uint8_t)lroundf(capability_decode(CAP_BATTERY_LEVEL, d.caps[CAP_BATTERY_LEVEL].raw))
            : 0xFF,
        .lux = d.caps[CAP_LIGHT_ILLUMINANCE].valid
            ? (uint32_t)lroundf(capability_decode(CAP_LIGHT_ILLUMINANCE, d.caps[CAP_LIGHT_ILLUMINANCE].raw))
            : 0xFFFFFFFFu,
        .conductivity_us = d.caps[CAP_SOIL_CONDUCTIVITY].valid
            ? (uint16_t)lroundf(capability_decode(CAP_SOIL_CONDUCTIVITY, d.caps[CAP_SOIL_CONDUCTIVITY].raw))
            : 0xFFFF,
        /* This node's own BLE signal to the sensor -- Task 1 (M5b) added
         * best_rssi to device_entry_t's attribution fields precisely so
         * this is available here. On a node, data_core_submit_from() is
         * only ever called locally with via_node == NULL (ble_collector
         * hears the sensor directly, same as on a hub), so best_rssi IS
         * this node's own reading of it, never another node's -- there is
         * no node-to-node relaying in M5b. This is the hub's "strongest
         * RSSI wins" attribution input (registry_attribute()); reporting a
         * hardcoded 0 here (as M5a did, before best_rssi existed) made
         * every node's contribution look identically weak and left that
         * attribution inert. */
        .rssi = d.best_rssi,
        .age_s = 0,  /* just heard */
        ._pad = 0,
    };
    memcpy(r->mac, mac, 6);

    if (!s_fwd_queue || xQueueSend(s_fwd_queue, &o, 0) != pdTRUE) {
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

/* Buffers a swarm_out_t that just failed to send. When full, the oldest
 * entry is evicted to make room -- logged at debug with a running counter,
 * per the brief, rather than silently discarding without any trace.
 *
 * M7 Task 5: the ring now carries any of the five outgoing tags, not just
 * READING -- the evicted entry's own tag decides what the log line can
 * usefully say about it: a MAC for a READING (the only tag with one), a
 * bare tag number for anything else (ANNOUNCE/GONE/MEASUREMENT/STATUS all
 * key on an 8-byte EUI-64 or carry none at all, neither of which is worth
 * a bespoke log line at DEBUG). */
static void buffer_push(const swarm_out_t *r, int64_t now_us)
{
    /* Coalesce onto an existing same-identity entry when possible (a bridge
     * node re-queues on every coex send timeout -- without this the same
     * reading piles up and is delivered to the hub many times, each rejected
     * by its no-regress guard; see swarm_buf_push_coalesce()). Only an
     * APPEND that finds the ring already full evicts the oldest, so the
     * "full" log belongs on that path alone. */
    bool was_full = swarm_buf_count(&s_buf) == SWARM_NODE_BUFFER_LEN;
    const swarm_out_t oldest = was_full ? s_buf.entries[s_buf.head].r : (swarm_out_t){0};
    bool coalesced = swarm_buf_push_coalesce(&s_buf, r, now_us);
    if (was_full && !coalesced) {
        if (oldest.tag == SWARM_OUT_READING) {
            ESP_LOGD(TAG, "forward buffer full (%d), dropped oldest READING for " MACSTR
                          " (dropped=%" PRIu32 " total)",
                     SWARM_NODE_BUFFER_LEN, MAC2STR(oldest.u.reading.mac),
                     swarm_buf_dropped(&s_buf));
        } else {
            ESP_LOGD(TAG, "forward buffer full (%d), dropped oldest entry (tag=%u) "
                          "(dropped=%" PRIu32 " total)",
                     SWARM_NODE_BUFFER_LEN, (unsigned)oldest.tag, swarm_buf_dropped(&s_buf));
        }
    }
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
    swarm_out_t r;
    int consec_fail = 0;
    /* One-shot: a working node->hub link is otherwise only inferable from
     * the hub side (frames_rx climbing in GET /api/v1/nodes) -- this makes
     * it positively visible on the node's own console the first time it
     * actually happens. */
    bool first_delivered = false;

    for (;;) {
        bool have_reading = xQueueReceive(s_fwd_queue, &r, 0) == pdTRUE;
        bool from_backlog = false;
        int64_t captured_us = 0;   /* only meaningful when from_backlog */

        if (!have_reading) {
            swarm_buf_entry_t br;
            if (swarm_buf_pop(&s_buf, &br)) {
                r = br.r;
                captured_us = br.captured_us;
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

        /* M7 Task 5: this ring/queue now carries any of five outgoing
         * shapes (swarm_frame.h's swarm_out_t) -- encode by tag rather than
         * always calling swarm_encode_reading(). age_s is recomputed here,
         * at transmit time, not at the moment an entry was (re)buffered --
         * the whole point of buffering is riding out an outage of unknown
         * length, so the hub must see how stale this entry actually is
         * right now. The stored age_s already carries whatever staleness
         * had accumulated before this buffering, so
         * swarm_buf_recompute_age() ADDS the additional wait on top of it,
         * compounding correctly across repeated buffer/retry cycles (and
         * clamping at UINT16_MAX rather than wrapping -- data_core's own
         * DATA_CORE_MAX_AGE_S (30 min) will drop a READING hub-side long
         * before that matters anyway). Only READING and MEASUREMENT carry
         * an age at all; ANNOUNCE/GONE/STATUS need no recomputation. */
        uint8_t buf[96];
        size_t n = 0;
        switch (r.tag) {
        case SWARM_OUT_READING:
            if (from_backlog) r.u.reading.age_s = swarm_buf_recompute_age(r.u.reading.age_s, captured_us, esp_timer_get_time());
            n = swarm_encode_reading(&r.u.reading, buf, sizeof buf);
            break;
        case SWARM_OUT_MEASUREMENT:
            /* I4 fix: this age_s is no longer a dead field on arrival -- the
             * hub's bridged-MEASUREMENT ingest (bridge_task's
             * SWARM_MSG_MEASUREMENT case) now recomputes total age from it
             * and drops the reading via data_core_submit_cap_id_aged() when
             * it exceeds DATA_CORE_MAX_AGE_S (1800 s), the same policy
             * data_core_submit_from() already applies to the BLE relay
             * path. 1800 s fits uint16 (max 65535) with room to spare, so
             * this narrowing cast never truncates a value the hub would
             * still accept. */
            if (from_backlog) r.u.meas.age_s = (uint16_t)swarm_buf_recompute_age(r.u.meas.age_s, captured_us, esp_timer_get_time());
            n = swarm_encode_measurement(&r.u.meas, buf, sizeof buf);
            break;
        case SWARM_OUT_ANNOUNCE:
            n = swarm_encode_device_announce(&r.u.ann, buf, sizeof buf);
            break;
        case SWARM_OUT_GONE:
            n = swarm_encode_device_gone(&r.u.gone, buf, sizeof buf);
            break;
        case SWARM_OUT_STATUS:
            n = swarm_encode_coord_status(&r.u.status, buf, sizeof buf);
            break;
        default:
            ESP_LOGW(TAG, "forward: unknown out tag %u, dropped", r.tag);
            continue;
        }
        if (n == 0) continue;

        esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
        note_hub_tx();
        if (err == ESP_OK) {
            consec_fail = 0;
            if (!first_delivered) {
                first_delivered = true;
                ESP_LOGI(TAG, "first frame delivered to hub (tag=%u)", (unsigned)r.tag);
                /* Node-side OTA rollback-guard health signal (M5c): the
                 * plan's primary criterion, "successfully delivered a
                 * reading to its hub" -- M7 Task 5 widens this to "any
                 * forwarded frame", since a zigbee-role node's very first
                 * successful delivery may well be an ANNOUNCE or STATUS
                 * rather than a READING; either is equally good proof this
                 * node's forward path works end to end. Only needs
                 * signalling once -- see signal_node_healthy()/
                 * ota_rollback_guard_node_confirm(), both idempotent past
                 * their first call -- so this rides the same
                 * first_delivered latch as the log line above rather than
                 * firing on every single successful send. */
                signal_node_healthy("frame delivered to hub");
            }
            continue;
        }

        if (err == ESP_ERR_TIMEOUT && radio_role_get() == RADIO_ROLE_ZIGBEE) {
            /* Bench finding (M7 gate 5, 2026-09-08): with the 802.15.4
             * coordinator up, the ESP-NOW send-done callback regularly lands
             * after the wait even for frames the hub did ingest. Re-queueing
             * on timeout replayed every reading 5-10x and tripped the resync
             * sweep (PING storm, node off-channel, hub commands failing). A
             * timeout is "delivered-unknown" on a bridge node. The damage in
             * the first storm was NOT the re-queued duplicates (the hub's
             * no-regress guard drops those) but the channel-resync sweep they
             * tripped, which took the node off-channel and starved hub->node
             * commands. So re-queue the reading (don't lose it) but do NOT
             * count it toward the resync threshold. Only an explicit ESP_FAIL
             * (the MAC reported no ACK) is a real failure that counts below. */
            ESP_LOGD(TAG, "frame send timed out (tag=%u); re-queued, not counted toward resync", (unsigned)r.tag);
            buffer_push(&r, esp_timer_get_time());
            /* Pace retries. A timed-out send on a bridge node is very likely
             * delivered (slow ACK under coex), so re-draining the backlog in
             * a tight loop just re-delivers the same reading 2-3x/s. Wait up
             * to BACKLOG_RETRY_GAP_MS for a FRESH reading -- which coalesces
             * onto the same backlog entry (swarm_buf_push_coalesce), so the
             * next drain sends the newest value, not a stale duplicate. An
             * actively-reporting sensor thus delivers at its own rate; a
             * quiet one retries every few seconds instead of continuously. */
            swarm_out_t fresh;
            if (xQueueReceive(s_fwd_queue, &fresh, pdMS_TO_TICKS(BACKLOG_RETRY_GAP_MS)) == pdTRUE)
                buffer_push(&fresh, esp_timer_get_time());
            continue;
        }

        consec_fail++;
        ESP_LOGW(TAG, "frame send failed (%s), tag=%u, consecutive=%d", esp_err_to_name(err), (unsigned)r.tag, consec_fail);
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

/* ---------------- Node side: zigbee bridge forwarding (M7 Task 5) ----------------
 *
 * Only ever wired up when this node's own radio_role_get() is
 * RADIO_ROLE_ZIGBEE -- a BLE-role node never calls zigbee_set_device_observer()/
 * zigbee_set_status_observer(), and neither does the hub's own build of this
 * file (swarm_start_main() has no equivalent of any of this): those callers
 * stay NULL, and zigbee.c's own call sites already guard on that (see its
 * header comment). "The hub never registers an observer" is therefore true
 * by construction, not by a special case here. */

/* zigbee.c's device-table observer. Fires on the caller's own task (the
 * stack task for a live join/re-interview, this node's own boot task via
 * zb_boot_replay_task() below for the store-restore replay, or a webserver
 * task for a rename/remove) -- per zigbee_set_device_observer()'s contract,
 * this must do nothing but build the frame and a non-blocking queue send,
 * never block and never call back into zigbee.c. */
static void zb_observer(const zb_device_t *dev, bool gone)
{
    swarm_out_t o = { .tag = gone ? SWARM_OUT_GONE : SWARM_OUT_ANNOUNCE };
    if (gone) {
        o.u.gone.dev.kind = DEV_KIND_ZIGBEE;
        memcpy(o.u.gone.dev.addr, dev->eui64, 8);
    } else {
        swarm_device_announce_t *a = &o.u.ann;
        a->dev.kind = DEV_KIND_ZIGBEE;
        memcpy(a->dev.addr, dev->eui64, 8);
        a->endpoint = dev->endpoint;
        a->interviewed = dev->interviewed;
        a->name_len = (uint8_t)strnlen(dev->name, SWARM_DEV_NAME_MAX);
        memcpy(a->name, dev->name, a->name_len);
        a->cap_count = dev->cap_count > SWARM_DEV_MAX_CAPS ? SWARM_DEV_MAX_CAPS : dev->cap_count;
        for (uint8_t i = 0; i < a->cap_count; i++) {
            a->cap_ids[i] = dev->caps[i];
            a->cap_clusters[i] = dev->cap_clusters[i];
        }
        a->action_count = dev->action_count > SWARM_DEV_MAX_ACTIONS ? SWARM_DEV_MAX_ACTIONS : dev->action_count;
        for (uint8_t i = 0; i < a->action_count; i++) a->action_ids[i] = dev->actions[i];
    }
    if (!s_fwd_queue || xQueueSend(s_fwd_queue, &o, 0) != pdTRUE)
        ESP_LOGW(TAG, "forward queue full, dropping %s", gone ? "device-gone" : "device-announce");
}

/* Builds a COORD_STATUS from zigbee.c's current state and queues it. Called
 * right after this node's own CHECKIN (always_on_checkin_task(), below --
 * a zigbee bridge cannot sleep, so that is its only CHECKIN path), once
 * after the boot replay (zb_boot_replay_task() below), and by
 * zb_status_observer() whenever zigbee.c reports the permit-join window
 * opening/closing or the network forming/restoring. */
static void queue_coord_status(void)
{
    uint8_t channel = 0;
    uint16_t pan_id = 0;
    bool formed = false;
    bool started = zigbee_net_info(&channel, &pan_id, &formed);

    swarm_out_t o = { .tag = SWARM_OUT_STATUS };
    o.u.status.radio_role = (uint8_t)radio_role_get();
    o.u.status.formed = started && formed;
    o.u.status.channel = channel;
    o.u.status.pan_id = pan_id;
    o.u.status.permit_s = zigbee_permit_join_remaining();
    o.u.status.device_count = (uint8_t)zigbee_device_count();
    if (!s_fwd_queue || xQueueSend(s_fwd_queue, &o, 0) != pdTRUE)
        ESP_LOGW(TAG, "forward queue full, dropping coordinator status");
}

/* zigbee.c's status-change observer -- see zigbee_set_status_observer()'s
 * header comment for exactly which transitions fire this. Same non-
 * blocking-only contract as zb_observer() above; queue_coord_status()
 * itself only ever does a bounded read of a few zigbee.c accessors plus one
 * xQueueSend, so this is safe to call directly rather than needing its own
 * wrapper. */
static void zb_status_observer(void)
{
    queue_coord_status();
}

/* One-shot boot replay: a zigbee-role node's forwarder queue starts empty,
 * but zigbee_start() (called by main.c right after swarm_start_node()
 * returns, per this node's own boot order) may already hold devices this
 * node remembers from a previous boot -- zb_register_restored_devices()
 * loads them into the store before zigbee_start() returns. Deliberately
 * NOT wired through the observer itself (zb_register_restored_devices()
 * calling zb_observer() per device would work too, but would make a
 * zigbee.c-internal restore loop responsible for this node's own
 * boot-announce policy); this task instead pulls the finished list itself,
 * once, the same way any other reader of zigbee_device_list() would.
 *
 * Polls zigbee_net_info()'s `started` return (true once zigbee_start()'s
 * xTaskCreate(zb_task) has succeeded, which -- per zigbee_start()'s own
 * comment -- is AFTER the store already loaded and restored devices are
 * already registered) rather than assuming a fixed delay: main.c's own
 * boot order guarantees zigbee_start() eventually runs when the role is
 * zigbee, but not how long swarm_start_node()'s own remaining work (this
 * task is created near its end) takes to return relative to it. Bounded so
 * a build where CONFIG_PLANTHUB_ZB_ENABLED is off (zigbee_net_info()
 * always returns false) does not spin forever. */
#define ZB_BOOT_REPLAY_POLL_MS   100u
#define ZB_BOOT_REPLAY_MAX_POLLS 100u  /* ~10s */

/* The actual replay: reads zigbee.c's current device list and re-announces
 * every entry through zb_observer(), same as a live join/re-interview
 * would. Factored out (M7 Task 6) so zb_boot_replay_task() below and
 * command_task()'s SWARM_CMD_RESYNC handling share the exact same replay
 * rather than two copies that could drift -- a hub-requested resync is
 * meant to reproduce the boot replay on demand, not a different, looser
 * approximation of it. Does not touch COORD_STATUS; both callers queue
 * that themselves right after, since a resync's status is current-state
 * (queue_coord_status()), never something this function needs to know
 * about. */
static void replay_announces(void)
{
    zb_device_t list[ZB_STORE_MAX_DEVICES];
    int n = zigbee_device_list(list, ZB_STORE_MAX_DEVICES);
    for (int d = 0; d < n; d++) zb_observer(&list[d], false);
    ESP_LOGI(TAG, "zigbee device replay: %d device(s) announced", n);
}

static void zb_boot_replay_task(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < ZB_BOOT_REPLAY_MAX_POLLS; i++) {
        if (zigbee_net_info(NULL, NULL, NULL)) {
            replay_announces();
            queue_coord_status();
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(ZB_BOOT_REPLAY_POLL_MS));
    }
    ESP_LOGW(TAG, "zigbee boot replay: gave up waiting for zigbee_start() to finish; "
                  "no boot announce/status sent this boot");
    vTaskDelete(NULL);
}

/* ---------------- Node side: command task (M7 Task 6) ----------------
 *
 * A zigbee-role bridge node's other half of the M7 bridge: where Task 5
 * only ever pushes data UP to the hub (announce/gone/measurement/status),
 * this is the hub pushing commands DOWN -- permit-join, actuate, remove,
 * rename, resync -- and this node acking each one back. s_cmd_queue is
 * filled by node_rx_cb's SWARM_MSG_COMMAND case above; s_ack_queue is
 * filled by on_zb_result() below, whenever zb_cmd_report() (zb_cmd.c)
 * reports an ACTUATE's outcome from the Zigbee stack task rather than
 * this task's own. */

/* Encodes and sends one SWARM_COMMAND_ACK to the hub. A failed send is
 * logged, not retried: the hub's own command retry (if it has one) is what
 * recovers a lost ack, the same posture node_config_task()'s
 * NODE_CONFIG_ACK send already takes just above. */
static void send_cmd_ack(uint16_t seq, uint8_t op, uint8_t status, uint8_t detail)
{
    swarm_command_ack_t a = { .seq = seq, .op = op, .status = status, .detail = detail };
    uint8_t buf[16];
    size_t  n = swarm_encode_command_ack(&a, buf, sizeof buf);
    if (!n) {
        ESP_LOGE(TAG, "COMMAND_ACK: failed to encode (seq=%u op=%u)", (unsigned)seq, (unsigned)op);
        return;
    }
    esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
    note_hub_tx();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "COMMAND_ACK send failed (%s), dropped -- the hub's own retry (if any) "
                      "recovers a lost ack", esp_err_to_name(err));
    }
}

/* An ACTUATE is the one command whose outcome is not known until later --
 * a ZCL Default Response, or zb_cmd.c's own timeout, both of which can be
 * seconds after actor_request() merely accepted it onto the queue. This is
 * the single-slot record of "which SWARM_CMD_ACTUATE are we still waiting
 * on", keyed by dev_idx: single-slot, not a table, because a second
 * ACTUATE is refused (SWARM_ACK_FAILED, detail 0xfe -- "busy") while one is
 * still pending, exactly so this never needs to track more than one. Only
 * ever written by command_task() itself (both the ACTUATE case that arms
 * it and on_zb_result() -- called synchronously, on this same task, for
 * every SYNCHRONOUS on_zb_dispatch() failure that routes through
 * zb_cmd_report() before a tsn is ever assigned; see command_task()'s call
 * to actor_service() below), except for the async completion path, which
 * only ever CLEARS `active`, never anything else -- so there is no real
 * race to guard here even though on_zb_result() can also run on the
 * Zigbee stack task.
 *
 * Unlocked on purpose: on_zb_result() runs on the Zigbee stack task (prio 5)
 * and command_task runs at prio 3, so the stack task can never be preempted
 * by command_task mid-update. This priority relationship is an invariant;
 * add a critical section if either priority changes. */
static struct { int dev_idx; uint16_t seq; bool active; int64_t deadline_us; } s_actuate_pending;
/* Since the local 10 s timeout below became a second writer of `active`
 * (command_task, prio 3, doing a read-then-clear that the prio-5 stack task
 * CAN preempt), the priority argument above no longer covers every path:
 * both writers take this spinlock around their read-modify-write so a real
 * result landing in the same microsecond as the timeout cannot produce two
 * acks for one seq. */
static portMUX_TYPE s_actuate_mux = portMUX_INITIALIZER_UNLOCKED;

/* zigbee.h's zb_cmd_result_t -- zb_cmd_report()'s one registered consumer
 * (registered below, in swarm_start_node()'s zigbee block, before
 * zigbee_start()). Fires on whichever task produced the outcome: the
 * Zigbee stack task for a real Default Response or a timeout, or
 * command_task()'s own task for a pre-dispatch failure (on_zb_dispatch()
 * calls zb_cmd_report() synchronously, from inside actor_service(), which
 * command_task() calls directly below) -- either way this must do nothing
 * but a non-blocking queue send, per zb_cmd_result_t's own contract, so
 * the actual espnow_link_send() always happens on command_task()'s task,
 * never here. */
static void on_zb_result(int dev_idx, uint8_t action_id, uint16_t param, bool ok, uint8_t zcl_status)
{
    (void)action_id;
    (void)param;
    taskENTER_CRITICAL(&s_actuate_mux);
    bool mine = s_actuate_pending.active && s_actuate_pending.dev_idx == dev_idx;
    uint16_t seq = s_actuate_pending.seq;
    if (mine) s_actuate_pending.active = false;
    taskEXIT_CRITICAL(&s_actuate_mux);
    if (!mine) {
        /* Not this node's one outstanding ACTUATE (already cleared by a
         * duplicate/late report or the local timeout, or an outcome for
         * some other dispatch entirely -- e.g. a rule/manual command
         * against the same device, which this bridge does not track
         * here). Nothing to ack. */
        return;
    }
    swarm_command_ack_t a = { .seq = seq, .op = SWARM_CMD_ACTUATE,
                              .status = ok ? SWARM_ACK_DONE : SWARM_ACK_FAILED, .detail = zcl_status };
    if (!s_ack_queue || xQueueSend(s_ack_queue, &a, 0) != pdTRUE)
        ESP_LOGW(TAG, "ack queue full/absent, dropping ACTUATE result (seq=%u)", (unsigned)a.seq);
}

/* Drains s_ack_queue (deferred acks for an ACTUATE outcome reported from
 * off-task) and s_cmd_queue (fresh COMMAND frames from the hub), and pumps
 * actor_service() -- the dispatch pump ble_collector.c's adv_decoder_task
 * runs for a BLE-role device, which a zigbee-role bridge node never starts
 * (main.c only calls ble_collector_start() for RADIO_ROLE_BLE), so without
 * this an ACTUATE would sit in actor.c's queue forever and never reach
 * zb_cmd.c's dispatch hook at all. Safe to call every pass: actor_service()
 * is a cheap no-op when its queue is empty, the same way it is for
 * ble_collector.c's caller.
 *
 * Duplicate suppression: `last_seq`/`last_op`/`last_status` remember only
 * the MOST RECENT command's outcome, not a history -- a hub that resends
 * the same seq (its own ack timeout/retry) gets the identical ack replayed
 * (detail forced to 0, since a replay's actual detail is a separate zb_cmd
 * result already reported once, not a full re-run of the command) rather
 * than the command being acted on twice; an ACTUATE in particular must
 * never re-issue a second actor_request() for a command already accepted.
 * A single remembered seq is enough because command_task() processes
 * s_cmd_queue strictly one at a time, so an out-of-order duplicate two-back
 * (rather than the immediately preceding command) is not a case the hub's
 * own single-outstanding-command-per-node discipline produces. */
static void command_task(void *arg)
{
    (void)arg;
    static uint16_t last_seq = 0xffff;
    static uint8_t  last_status = 0, last_op = 0;
    for (;;) {
        actor_service();

        /* I1 fix: s_actuate_pending is a single-slot latch that would
         * otherwise never clear if actor_service()'s TTL-drop or
         * service-time-decline paths swallow the outcome without ever
         * reaching on_zb_result() (see the struct's comment above) --
         * every subsequent ACTUATE would then be refused (detail 0xfe)
         * forever. A local 10 s deadline, checked every pass through this
         * already-periodic loop, guarantees the slot always frees itself
         * even if no zb_cmd result ever arrives. 0xfd marks a local
         * timeout, distinct from zb_cmd.c's own 0xff wire timeout. */
        {
            int64_t now_us = esp_timer_get_time();
            taskENTER_CRITICAL(&s_actuate_mux);
            bool timed_out = s_actuate_pending.active && now_us >= s_actuate_pending.deadline_us;
            uint16_t timed_out_seq = s_actuate_pending.seq;
            if (timed_out) s_actuate_pending.active = false;
            taskEXIT_CRITICAL(&s_actuate_mux);
            if (timed_out) send_cmd_ack(timed_out_seq, SWARM_CMD_ACTUATE, SWARM_ACK_FAILED, 0xfd);
        }

        swarm_command_ack_t queued;
        if (xQueueReceive(s_ack_queue, &queued, 0) == pdTRUE) {
            send_cmd_ack(queued.seq, queued.op, queued.status, queued.detail);
            continue;
        }

        swarm_command_t c;
        if (xQueueReceive(s_cmd_queue, &c, pdMS_TO_TICKS(200)) != pdTRUE) continue;

        if (c.seq == last_seq) {
            send_cmd_ack(c.seq, last_op, last_status, 0);
            continue;
        }

        /* ttl_s is informational here, not enforced: this task cannot know
         * how long a COMMAND frame sat anywhere before being decoded (no
         * receipt timestamp travels with it), so there is no honest way to
         * treat it as already-expired and drop it silently. Every decoded
         * command gets an ack, always -- ACTUATE alone forwards ttl_s
         * onward, as actor_request()'s own deadline_s, where actor.c's
         * queue (which DOES know when "now" is on this device) enforces it
         * for real. */
        uint8_t status = SWARM_ACK_FAILED, detail = 0;
        switch (c.op) {
        case SWARM_CMD_PERMIT_JOIN:
            /* c.arg is NOT consulted here: zigbee.h's zigbee_permit_join()
             * takes no duration argument -- it always opens for
             * CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S and has no "close now" entry
             * point, and widening its signature would also have to touch
             * api_v1.c's own PERMIT_JOIN route (out of this task's file
             * list, and off-limits). So a hub-requested permit-join always
             * opens (or re-opens) for the compiled-in duration regardless
             * of what `arg` asked for; c.arg == 0 does NOT close an
             * already-open window early. The STATUS frame this bridge
             * already sends on open/close (Task 5) still carries the real
             * countdown either way, so the hub's UI is never told a wrong
             * duration -- only a hub that specifically wants a SHORTER or
             * an early-closed window than the compiled-in default does not
             * get that today. */
            status = zigbee_permit_join() ? SWARM_ACK_DONE : SWARM_ACK_FAILED;
            break;
        case SWARM_CMD_DEVICE_REMOVE: {
            /* I5 fix: this is a pure existence check -- zigbee_store_lookup()
             * (already used by zb_cmd.c) answers it with no stack array at
             * all, unlike a zb_device_t[ZB_STORE_MAX_DEVICES] copy (~1 KB)
             * used only to linear-scan for a match. NULL out params: the
             * short_addr/endpoint it could also return are not needed here. */
            if (!zigbee_store_lookup(c.dev.addr, NULL, NULL)) { detail = 1; break; }
            status = zigbee_device_remove(c.dev.addr) ? SWARM_ACK_DONE : SWARM_ACK_FAILED;
            if (status != SWARM_ACK_DONE) detail = 2;
            break; }
        case SWARM_CMD_DEVICE_RENAME: {
            /* I5 fix: same existence-check swap as DEVICE_REMOVE above. */
            if (!zigbee_store_lookup(c.dev.addr, NULL, NULL)) { detail = 1; break; }
            char name[SWARM_DEV_NAME_MAX + 1];
            uint8_t name_len = c.name_len > SWARM_DEV_NAME_MAX ? SWARM_DEV_NAME_MAX : c.name_len;
            memcpy(name, c.name, name_len);
            name[name_len] = '\0';
            status = zigbee_device_rename(c.dev.addr, name) ? SWARM_ACK_DONE : SWARM_ACK_FAILED;
            if (status != SWARM_ACK_DONE) detail = 2;
            break; }
        case SWARM_CMD_RESYNC:
            replay_announces();
            queue_coord_status();
            status = SWARM_ACK_DONE;
            break;
        case SWARM_CMD_ACTUATE: {
            if (s_actuate_pending.active) {
                /* Single-slot pending record, already occupied: refuse
                 * rather than clobber the ACTUATE this bridge is already
                 * waiting on a result for. */
                detail = 0xfe;
                break;
            }
            device_id_t id = { .kind = DEV_KIND_ZIGBEE };
            memcpy(id.addr, c.dev.addr, 8);
            int idx = data_core_find_index(&id);
            if (idx < 0) {
                /* Vanished between the hub last seeing it announced and
                 * this command arriving (removed, or never actually
                 * registered on this bridge) -- FAILED, not a crash. */
                detail = 1;
                break;
            }
            taskENTER_CRITICAL(&s_actuate_mux);
            s_actuate_pending = (typeof(s_actuate_pending)){ .dev_idx = idx, .seq = c.seq, .active = true,
                                                              .deadline_us = esp_timer_get_time() + 10 * 1000000LL };
            taskEXIT_CRITICAL(&s_actuate_mux);
            bool queued_ok = actor_request(idx, (uint8_t)(c.arg & 0xff), (uint16_t)(c.arg >> 8),
                                            ACTOR_SRC_REMOTE, actor_now_s() + c.ttl_s);
            if (!queued_ok) {
                taskENTER_CRITICAL(&s_actuate_mux);
                s_actuate_pending.active = false;
                taskEXIT_CRITICAL(&s_actuate_mux);
                detail = 2;
                break;
            }
            status = SWARM_ACK_ACCEPTED;
            break; }
        default:
            break;
        }

        last_seq = c.seq;
        last_op = c.op;
        last_status = status;
        send_cmd_ack(c.seq, c.op, status, detail);
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
    note_hub_tx();
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
/* M7 zigbee bridge follow-up: this bench's finding is that a zigbee-role
 * node's WiFi receive path only reliably opens right after this node's OWN
 * transmit (WiFi/802.15.4 coexistence, coex priority config already at its
 * most permissive defaults) -- so a hub->node command sent "cold" is
 * dropped (espnow_link_send() ESP_FAIL, no MAC ack) unless it happens to
 * land within roughly a millisecond of this node's own last send. A
 * bridge node that has nothing new to forward for a while (no fresh
 * ANNOUNCE/MEASUREMENT/CHECKIN/ack) never reopens that window on its own,
 * so any command the hub queues in the meantime just sits until the
 * periodic retry (bridge_task's router tick, swarm.c hub side) happens to
 * line up -- which this bench showed does not reliably happen within a
 * command's TTL.
 *
 * This task manufactures that opening on a fixed cadence instead of
 * waiting for one: every SWARM_POLL_INTERVAL_US, if this node has not
 * sent ANYTHING ELSE to the hub in that same window (s_last_hub_tx_us,
 * updated by note_hub_tx() at every node->hub send site in this file), it
 * sends an empty SWARM_MSG_POLL purely to trigger a transmit -- the POLL
 * itself carries no data and gets no reply. hub_rx_cb's POLL branch (hub
 * side) posts a BRIDGE_ITEM_FLUSH for this mac the moment that POLL (or
 * any other accepted frame) arrives, and bridge_task's FLUSH case is what
 * actually seizes the resulting window to push a pending command through,
 * via bridge_cmd_peek_pending()/bridge_cmd_mark_sent() (bridge_cmd.h). */
#define SWARM_POLL_INTERVAL_US (2 * 1000000)

static void poll_task(void *arg)
{
    (void)arg;
    espnow_link_set_send_wait_ms(1000);  /* see espnow_link.h: 802.15.4 coex delays the send-done callback */
    ESP_LOGI(TAG, "poll task started, 2 s; send-done wait 1000 ms");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SWARM_POLL_INTERVAL_US / 1000));

        if (esp_timer_get_time() - s_last_hub_tx_us < SWARM_POLL_INTERVAL_US) continue;

        uint8_t buf[SWARM_HDR_LEN];
        size_t n = swarm_encode_poll(buf, sizeof buf);
        if (n == 0) continue;
        esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
        note_hub_tx();
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "POLL send failed: %s", esp_err_to_name(err));
        }
    }
}

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

        /* M7 Task 5: a zigbee bridge sends its coordinator status right
         * after every CHECKIN -- this is its only CHECKIN path (a
         * coordinator cannot sleep, so swarm_node_battery_cycle()'s own
         * checkin path never runs for it, see main.c's batt_cycle_task
         * gating). A non-zigbee node sends no STATUS at all. */
        if (radio_role_get() == RADIO_ROLE_ZIGBEE) queue_coord_status();

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

    /* Same "must exist before node_rx_cb can call it" reasoning as the
     * queues just above -- see s_node_cfg_queue's own comment. */
    if (!s_node_cfg_queue) s_node_cfg_queue = xQueueCreate(1, sizeof(swarm_node_config_t));
    if (!s_node_cfg_queue) {
        ESP_LOGE(TAG, "swarm_start_node: failed to create NODE_CONFIG queue; hub-initiated radio "
                      "role changes will never be delivered this boot");
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
     * regulatory domain to CONFIG_PLANTHUB_WIFI_COUNTRY (or, M9, an
     * operator-set region -- see below) as its default. If this node has
     * previously learned a different country from the hub's PAIR_ACK
     * (protocol v2+), re-apply THAT here instead -- every boot, not just
     * the one right after pairing -- so a resync's channel sweep
     * (pairing_node_resync_channel(), below this in the boot sequence via
     * forward_task()) can actually reach channels 12-13 if the hub's
     * domain allows them, rather than silently reverting to the compile-
     * time default on every restart. Absent (this node paired under v1, or
     * was factory-reset) just means "nothing to reapply" -- the default
     * espnow_link_init() already set stands, same as a pre-v2 node. Same
     * MANUAL policy reasoning as pairing.c: this device never associates,
     * so ieee80211d_enabled stays false always.
     *
     * (M9) Precedence check repeated here, not just in espnow_link.c: an
     * operator-set region on THIS node already won there and is what's
     * currently applied. Re-applying the learned hub_cc unconditionally
     * would silently overwrite that explicit choice with router hearsay on
     * every single boot -- the one precedence violation that wouldn't be
     * visible in espnow_link.c's own log line, since this call happens
     * strictly after it returns. Skip the reapply entirely when a user
     * region is set; swarm_store_region() is a cheap RAM-cache read, so
     * this costs nothing on the common (no user region) path. */
    char probe_region[3];
    if (swarm_store_region(probe_region)) {
        ESP_LOGI(TAG, "user region %s set -- not overriding it with the learned hub country",
                 probe_region);
    } else {
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
    s_fwd_queue = xQueueCreate(SWARM_FWD_QUEUE_LEN, sizeof(swarm_out_t));
    if (!s_fwd_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(forward_task, "swarm_fwd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create forward task");
        return ESP_ERR_NO_MEM;
    }

    /* M7 Task 5: only a zigbee-role node wires itself into zigbee.c's
     * observers and replays its stored device table -- a BLE-role node has
     * nothing to observe (its own radio_role_get() never even starts the
     * zigbee stack, see main.c), and neither does the hub's own build of
     * this file. Registered here, with s_fwd_queue already created just
     * above and BEFORE main.c's own zigbee_start() call (which always runs
     * after swarm_start_node() returns, per this file's boot-order
     * comments elsewhere) -- so neither observer can ever fire into a NULL
     * queue. */
    if (radio_role_get() == RADIO_ROLE_ZIGBEE) {
        zigbee_set_device_observer(zb_observer);
        zigbee_set_status_observer(zb_status_observer);
        if (xTaskCreate(zb_boot_replay_task, "swarm_zb_replay", 3072, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create zigbee boot replay task; the hub will not learn "
                          "this bridge's already-known devices until they next announce");
        }

        /* M7 Task 6: a zigbee-role bridge never calls ble_collector_start()
         * (main.c starts exactly one of BLE/Zigbee per node -- see
         * radio_role.h), and actor_init() is otherwise called ONLY from
         * there -- so without this, the shared actor table this device's
         * own zigbee.c uses (actor_declare() at every device
         * interview/restore) would stay raw BSS zero, where every row's
         * dev_idx reads as 0 (not actor_table_init()'s -1 "free" sentinel)
         * and actor_declare()/actor_request() silently fail or misattribute
         * for any device other than index 0. Called here, before
         * zigbee_start() (always the caller's very next step after this
         * function returns -- see this file's own boot-order comments),
         * so the table is ready before zb_register_restored_devices()
         * makes its first actor_declare() call. actor_persist_init()/
         * pending_close_init() are deliberately NOT called here: neither
         * ACT_SWITCH_ON nor ACT_SWITCH_OFF (the only actions zb_cmd.c
         * implements) ever arms a pending close (see zb_cmd.c's own top
         * comment), and this bridge has no HTTP API of its own to persist
         * operator-configured guards through a reboot. */
        actor_init();

        /* Same "must exist before node_rx_cb/command_task can use it"
         * eager-init reasoning as s_checkin_ack_queue/s_node_cfg_queue
         * above -- see s_cmd_queue/s_ack_queue's own comments. Only
         * created on a zigbee-role node: the hub never sends a COMMAND to
         * a BLE-role node, so neither queue is ever needed there. */
        if (!s_cmd_queue) s_cmd_queue = xQueueCreate(4, sizeof(swarm_command_t));
        if (!s_ack_queue) s_ack_queue = xQueueCreate(1, sizeof(swarm_command_ack_t));
        if (!s_cmd_queue || !s_ack_queue) {
            ESP_LOGE(TAG, "failed to create COMMAND queue(s); hub-initiated permit-join/"
                          "actuate/remove/rename/resync will never be delivered this boot");
        }

        /* zb_cmd_result_t's one registrant (see zigbee.h's own doc
         * comment) -- registered before zigbee_start() so no ACTUATE
         * dispatched right after the network forms can ever report through
         * an unset callback. */
        zb_cmd_set_result_cb(on_zb_result);

        /* I5 fix: 4096, up from 3072 -- replay_announces() (SWARM_CMD_RESYNC)
         * still allocates one zb_device_t[ZB_STORE_MAX_DEVICES] (~1 KB) on
         * top of zb_observer()'s own frame plus ESP-IDF log formatting; the
         * two existence-check arrays this task used to carry alongside it
         * are gone (see DEVICE_REMOVE/DEVICE_RENAME above), but this stays a
         * real margin rather than a recount back down to the old number. */
        if (xTaskCreate(command_task, "swarm_cmd", 4096, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create command task; hub-initiated permit-join/actuate/"
                          "remove/rename/resync will never be delivered this boot");
        }

        /* M7 zigbee bridge follow-up: only a zigbee-role node needs to
         * manufacture its own post-TX WiFi receive window this way -- see
         * poll_task's own comment for the bench finding behind it. Failure
         * here is logged, not fatal to starting as a node: forwarding/
         * pairing/OTA and the periodic router-tick retry all still work,
         * only the early-flush path a POLL enables goes missing until this
         * node's next reboot. */
        if (xTaskCreate(poll_task, "swarm_poll", 2048, NULL, 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create poll task; hub-initiated commands will only be "
                          "delivered on this bridge's own periodic retry timer");
        }
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

    /* M7 Task 4: same "started unconditionally, failure logged not fatal"
     * shape as always_on_checkin_task() just above -- a node running BLE or
     * Zigbee both need to be reachable for a hub-initiated radio-role
     * switch. */
    if (xTaskCreate(node_config_task, "swarm_nodecfg", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create node config task; hub-initiated radio role changes "
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
