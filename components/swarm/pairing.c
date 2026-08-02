/* Pairing handshake: hub-side adoption window and node-side channel sweep.
 *
 * CRITICAL: swarm_store_add_node()/swarm_store_set_hub() perform a
 * synchronous NVS/flash commit, and espnow_link_send()/broadcast() block on
 * a semaphore signalled by the ESP-NOW send callback — which runs on the
 * same WiFi driver task as the ESP-NOW receive callback. None of the four
 * may be called from pairing_handle_frame() (which IS the receive-callback
 * path): a flash write there would stall the radio for milliseconds, and a
 * send there would have the WiFi task waiting on a signal only itself can
 * deliver. So pairing_handle_frame() only ever validates a frame, stashes a
 * few fields under a short-lived lock, and signals a queue/semaphore that a
 * dedicated FreeRTOS task (created by this file, not the WiFi task) is
 * blocked on. That task does every flash write and every send. */
#include "pairing.h"
#include "espnow_link.h"
#include "swarm_frame.h"
#include "swarm_store.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "pairing";

/* ---------------- Hub side ---------------- */

typedef struct {
    uint8_t  mac[6];
    uint32_t nonce;
} pending_adopt_t;

static SemaphoreHandle_t s_window_lock;   /* guards s_window_open / s_window_deadline_us / s_adopt_in_progress */
static volatile bool s_window_open;
static int64_t s_window_deadline_us;
static volatile bool s_adopt_in_progress; /* an adoption is queued/running; blocks a 2nd PAIR_REQ */

static QueueHandle_t s_adopt_queue;       /* depth 1: one pending adoption at a time */
static TaskHandle_t s_hub_task;

static void hub_task(void *arg);

/* Returns false if the hub task (or its supporting primitives) could not be
 * created. Checking this matters: if xTaskCreate() silently failed here, a
 * PAIR_REQ accepted into s_adopt_queue by pairing_handle_frame() would sit
 * there forever with nothing to drain it, leaving s_adopt_in_progress stuck
 * true and pairing permanently impossible until reboot. So the caller must
 * refuse to open a window rather than open one nothing can service. */
static bool ensure_hub_task(void)
{
    if (s_hub_task) return true;
    if (!s_window_lock) s_window_lock = xSemaphoreCreateMutex();
    if (!s_adopt_queue) s_adopt_queue = xQueueCreate(1, sizeof(pending_adopt_t));
    if (!s_window_lock || !s_adopt_queue) {
        ESP_LOGE(TAG, "ensure_hub_task: failed to allocate window lock/adopt queue");
        return false;
    }
    BaseType_t ok = xTaskCreate(hub_task, "pairing_hub", 4096, NULL, 5, &s_hub_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ensure_hub_task: xTaskCreate(pairing_hub) failed");
        s_hub_task = NULL;
        return false;
    }
    return true;
}

void pairing_open_window(uint32_t seconds)
{
    if (!ensure_hub_task()) {
        ESP_LOGE(TAG, "pairing_open_window: hub task unavailable, window NOT opened");
        return;
    }

    xSemaphoreTake(s_window_lock, portMAX_DELAY);
    s_window_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_window_open = true;
    s_adopt_in_progress = false; /* defensive: a fresh window always starts idle */
    xSemaphoreGive(s_window_lock);

    /* Logging the channel here makes a hub/node channel mismatch visible
     * at a glance in the console instead of having to infer it from
     * separate hub and node logs. */
    ESP_LOGI(TAG, "pairing window open for %" PRIu32 "s on channel %u", seconds, espnow_link_channel());
}

bool pairing_window_open(void)
{
    if (!s_window_lock) return false;

    xSemaphoreTake(s_window_lock, portMAX_DELAY);
    bool open = s_window_open;
    if (open && esp_timer_get_time() >= s_window_deadline_us) {
        s_window_open = false;
        open = false;
    }
    xSemaphoreGive(s_window_lock);
    return open;
}

uint32_t pairing_window_remaining_s(void)
{
    if (!pairing_window_open()) return 0;

    xSemaphoreTake(s_window_lock, portMAX_DELAY);
    int64_t remaining_us = s_window_deadline_us - esp_timer_get_time();
    xSemaphoreGive(s_window_lock);
    return remaining_us > 0 ? (uint32_t)(remaining_us / 1000000) : 0;
}

/* Looks up `mac` in swarm_store's persisted node table. Returns true and
 * fills lmk_out if found. Used to make adoption idempotent: a node whose
 * first PAIR_ACK never arrived keeps sweeping and re-sending PAIR_REQ from
 * the SAME MAC it was already adopted under, and it must be re-acked with
 * that same stored key, never a freshly generated one -- a new key here
 * would desync it from the key already committed to flash. */
static bool find_stored_lmk(const uint8_t mac[6], uint8_t lmk_out[SWARM_LMK_LEN])
{
    int n = swarm_store_node_count();
    for (int i = 0; i < n; i++) {
        uint8_t stored_mac[6];
        if (swarm_store_node_at(i, stored_mac, lmk_out) && memcmp(stored_mac, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* Runs on its own task, never on the WiFi task: safe to touch NVS and to
 * call the blocking espnow_link_send(). */
static void hub_task(void *arg)
{
    (void)arg;
    pending_adopt_t item;

    for (;;) {
        if (xQueueReceive(s_adopt_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        uint8_t lmk[SWARM_LMK_LEN];
        bool already_adopted = find_stored_lmk(item.mac, lmk);
        if (!already_adopted) esp_fill_random(lmk, sizeof(lmk));

        do {
            uint8_t channel = espnow_link_channel();

            /* PAIR_ACK carries the LMK itself, so it MUST go out
             * UNENCRYPTED -- the node cannot possibly hold the key yet,
             * since delivering that key is the entire point of this
             * frame. swarm_frame.h documents PAIR_ACK as "Hub -> node,
             * unicast, plaintext" for exactly this reason: do NOT
             * "harden" this by adding the peer encrypted before sending,
             * that breaks pairing outright. Confirmed on real hardware:
             * an encrypted-before-send ack got ESP_FAIL (ESP_NOW_SEND_FAIL)
             * on every attempt, because a peer that doesn't have the LMK
             * cannot decrypt -- and therefore cannot 802.11-ack -- a frame
             * that was encrypted with it. So: add the peer unencrypted
             * first, send, and only once that plaintext send is confirmed
             * delivered do we know the node has the key and it's safe to
             * upgrade the peer to encrypted on our own side. */
            esp_err_t err = espnow_link_add_peer(item.mac, NULL, 0);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "adopt " MACSTR ": espnow_link_add_peer (plaintext) failed: %s",
                         MAC2STR(item.mac), esp_err_to_name(err));
                break;
            }

            swarm_pair_ack_t ack = {
                .version = SWARM_PROTO_VERSION,
                .type = SWARM_MSG_PAIR_ACK,
                .channel = channel,
                .nonce = item.nonce,
            };
            memcpy(ack.lmk, lmk, sizeof(lmk));

            uint8_t buf[sizeof(ack)];
            size_t n = swarm_encode_pair_ack(&ack, buf, sizeof(buf));
            if (n == 0) {
                ESP_LOGE(TAG, "adopt " MACSTR ": failed to encode PAIR_ACK", MAC2STR(item.mac));
                break;
            }

            /* Send BEFORE persisting: swarm_store_add_node() below is a
             * synchronous, multi-millisecond NVS/flash commit, and on real
             * hardware that latency was observed to exceed the node's
             * per-channel sweep dwell -- delaying the ack behind it meant
             * the node had already hopped to the next channel by the time
             * the ack would have gone out, producing a one-sided pairing
             * (hub adopted + persisted, node never heard back). Sending
             * first means a slow or failed persist can no longer cost the
             * node a heard ack. */
            err = espnow_link_send(item.mac, buf, n);
            /* Observability: this handshake is otherwise invisible on the
             * console, which cost real diagnostic time tracking down a
             * one-sided pairing defect. Log every attempt's outcome, not
             * just failures. */
            ESP_LOGI(TAG, "PAIR_ACK -> " MACSTR " channel=%u result=%s",
                     MAC2STR(item.mac), channel, esp_err_to_name(err));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "adopt " MACSTR ": PAIR_ACK send failed: %s",
                         MAC2STR(item.mac), esp_err_to_name(err));
                break;
            }

            /* The node just accepted (and 802.11-acked) a plaintext frame
             * containing the LMK, so it now has the key too -- safe to
             * upgrade this peer to encrypted on our side. espnow_link_add_peer()
             * calls esp_now_mod_peer() in place when the peer already
             * exists (verified against this IDF's esp_now.h: esp_now_mod_peer()
             * is available), so this is the same call as above, just with
             * a non-NULL lmk this time -- no del+re-add needed. */
            err = espnow_link_add_peer(item.mac, lmk, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "adopt " MACSTR ": failed to upgrade peer to encrypted (%s); removing peer",
                         MAC2STR(item.mac), esp_err_to_name(err));
                espnow_link_remove_peer(item.mac);
                break;
            }

            if (already_adopted) {
                ESP_LOGI(TAG, "re-acked already-adopted node " MACSTR " on channel %u",
                         MAC2STR(item.mac), channel);
            } else {
                err = swarm_store_add_node(item.mac, lmk);
                if (err != ESP_OK) {
                    /* The node now has a working ESP-NOW peer and just
                     * heard an ack for a key we failed to remember. If we
                     * left the peer in place, this device would think
                     * it isn't paired (swarm_store says so) while the
                     * node thinks it is -- silently disagreeing about the
                     * key. Remove the peer so both sides are consistently
                     * "not paired"; the node will keep sweeping and this
                     * PAIR_REQ will simply be retried as a fresh, non-
                     * idempotent attempt next time. */
                    ESP_LOGE(TAG, "adopt " MACSTR ": swarm_store_add_node failed (%s); removing peer",
                             MAC2STR(item.mac), esp_err_to_name(err));
                    espnow_link_remove_peer(item.mac);
                    break;
                }
                ESP_LOGI(TAG, "adopted node " MACSTR " on channel %u", MAC2STR(item.mac), channel);
            }
        } while (0);

        /* This attempt is no longer "in progress", so a fresh PAIR_REQ
         * (from this node retrying, or another one) can be queued. The
         * window is never closed here, on success or failure -- it stays
         * open for its full configured duration (see pairing_open_window())
         * so a node that doesn't hear this ack can keep retrying instead
         * of finding the window gone by the time it sweeps back around. */
        xSemaphoreTake(s_window_lock, portMAX_DELAY);
        s_adopt_in_progress = false;
        xSemaphoreGive(s_window_lock);
    }
}

/* ---------------- Node side ---------------- */

static SemaphoreHandle_t s_node_lock;     /* guards s_node_nonce / s_node_ack / s_node_ack_mac */
static SemaphoreHandle_t s_node_ack_sem;  /* signalled by pairing_handle_frame on a matching ack */
static TaskHandle_t s_node_task;
static volatile pairing_state_t s_node_state = PAIR_IDLE;
static uint32_t s_node_nonce;
static swarm_pair_ack_t s_node_ack;
static uint8_t s_node_ack_mac[6];

typedef struct {
    uint32_t timeout_s;
} node_task_args_t;

static void node_task(void *arg);

esp_err_t pairing_node_start(uint32_t timeout_s)
{
    if (s_node_task) return ESP_ERR_INVALID_STATE;

    if (!s_node_lock) s_node_lock = xSemaphoreCreateMutex();
    if (!s_node_ack_sem) s_node_ack_sem = xSemaphoreCreateBinary();
    if (!s_node_lock || !s_node_ack_sem) return ESP_ERR_NO_MEM;

    static node_task_args_t args; /* single sweep at a time; lifetime = task's */
    args.timeout_s = timeout_s;

    s_node_state = PAIR_SEARCHING;
    BaseType_t ok = xTaskCreate(node_task, "pairing_node", 4096, &args, 5, &s_node_task);
    if (ok != pdPASS) {
        s_node_state = PAIR_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

pairing_state_t pairing_node_state(void)
{
    return s_node_state;
}

static void node_task(void *arg)
{
    node_task_args_t args = *(node_task_args_t *)arg;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)args.timeout_s * 1000000;
    /* Widened from 300ms: on real hardware, a hub's PAIR_ACK (queued behind
     * a synchronous NVS commit in the old ordering) could arrive after a
     * 300ms dwell had already moved the sweep to the next channel. The hub
     * now sends before persisting (see hub_task()), but the wider dwell
     * stays as an independent margin. The sweep is bounded by deadline_us
     * above, not a fixed channel/sweep count, so widening this simply
     * means fewer full 1..13 sweeps fit in the same timeout_s -- no
     * separate count needs to change to match. */
    const TickType_t dwell_ticks = pdMS_TO_TICKS(500);

    while (esp_timer_get_time() < deadline_us) {
        uint32_t nonce = esp_random();
        xSemaphoreTake(s_node_lock, portMAX_DELAY);
        s_node_nonce = nonce;
        xSemaphoreGive(s_node_lock);
        xSemaphoreTake(s_node_ack_sem, 0); /* drop any stale signal */

        for (uint8_t ch = 1; ch <= 13 && esp_timer_get_time() < deadline_us; ch++) {
            esp_err_t cherr = espnow_link_set_channel(ch);
            if (cherr != ESP_OK) {
                ESP_LOGW(TAG, "sweep: set_channel(%u) failed: %s", ch, esp_err_to_name(cherr));
                continue;
            }
            ESP_LOGD(TAG, "sweep: dwelling on channel %u", ch);

            swarm_pair_req_t req = {
                .version = SWARM_PROTO_VERSION,
                .type = SWARM_MSG_PAIR_REQ,
                .nonce = nonce,
            };
            uint8_t buf[sizeof(req)];
            size_t n = swarm_encode_pair_req(&req, buf, sizeof(buf));

            TickType_t dwell_start = xTaskGetTickCount();
            if (n) espnow_link_broadcast(buf, n);
            TickType_t elapsed = xTaskGetTickCount() - dwell_start;
            TickType_t remaining = (elapsed < dwell_ticks) ? (dwell_ticks - elapsed) : 0;

            if (xSemaphoreTake(s_node_ack_sem, remaining) == pdTRUE) {
                swarm_pair_ack_t ack;
                uint8_t hub_mac[6];
                xSemaphoreTake(s_node_lock, portMAX_DELAY);
                ack = s_node_ack;
                memcpy(hub_mac, s_node_ack_mac, 6);
                xSemaphoreGive(s_node_lock);

                esp_err_t serr = swarm_store_set_hub(hub_mac, ack.lmk, ack.channel);
                if (serr == ESP_OK) {
                    espnow_link_add_peer(hub_mac, ack.lmk, 0);
                    ESP_LOGI(TAG, "paired to hub " MACSTR " on channel %u",
                             MAC2STR(hub_mac), ack.channel);
                    s_node_state = PAIR_OK;
                } else {
                    ESP_LOGE(TAG, "swarm_store_set_hub failed: %s", esp_err_to_name(serr));
                    s_node_state = PAIR_FAILED;
                }
                s_node_task = NULL;
                vTaskDelete(NULL);
                return;
            }
        }
    }

    ESP_LOGW(TAG, "pairing timed out after %" PRIu32 "s", args.timeout_s);
    s_node_state = PAIR_FAILED;
    s_node_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- Shared receive path ---------------- */

void pairing_handle_frame(const uint8_t src[6], const uint8_t *data, int len, int rssi)
{
    (void)rssi;
    if (!src || !data || len <= 0) return;

    int type = swarm_frame_type(data, (size_t)len);

    if (type == SWARM_MSG_PAIR_REQ) {
        bool win_open = pairing_window_open();
        /* Observability: this is the single most useful line for
         * diagnosing a one-sided pairing -- it proves the hub's receiver
         * actually heard the request at all, and whether the window was
         * open for it. Cheap (one log call, no NVS/flash, no send), so
         * safe to leave on the receive-callback path. */
        ESP_LOGI(TAG, "PAIR_REQ from " MACSTR " (window %s)", MAC2STR(src), win_open ? "open" : "closed");
        if (!win_open) return;

        swarm_pair_req_t req;
        if (!swarm_decode_pair_req(data, (size_t)len, &req)) return;

        /* At most one adoption in flight at a time. The window is never
         * closed here (or anywhere on a successful adoption -- see
         * hub_task()): it stays open for its full configured duration so a
         * node whose PAIR_ACK gets lost can retry within the same window
         * instead of finding it already gone. */
        xSemaphoreTake(s_window_lock, portMAX_DELAY);
        bool busy = s_adopt_in_progress;
        if (!busy) s_adopt_in_progress = true;
        xSemaphoreGive(s_window_lock);
        if (busy) return;

        pending_adopt_t item;
        memcpy(item.mac, src, 6);
        item.nonce = req.nonce;

        if (!s_adopt_queue || xQueueSend(s_adopt_queue, &item, 0) != pdTRUE) {
            /* Could not hand off to hub_task (queue missing, or somehow
             * already full despite the in-progress guard): release the
             * guard so a later PAIR_REQ in this window can try again. */
            xSemaphoreTake(s_window_lock, portMAX_DELAY);
            s_adopt_in_progress = false;
            xSemaphoreGive(s_window_lock);
        }
        return;
    }

    if (type == SWARM_MSG_PAIR_ACK) {
        if (s_node_state != PAIR_SEARCHING) return;

        swarm_pair_ack_t ack;
        if (!swarm_decode_pair_ack(data, (size_t)len, &ack)) return;

        /* Accepted risk: this ack is matched to our request by nonce alone,
         * and the nonce was just broadcast in clear as part of our own
         * PAIR_REQ. Any hostile device in radio range during the window
         * could race the real hub and win by replying first with a bogus
         * LMK. That's the same trust tier as the cleartext LMK the hub
         * sends back here — the mitigation is procedural, not
         * cryptographic: the window is short-lived and only ever open
         * because a human operator deliberately started it, so the
         * exposure for such a race is small and operator-visible. */

        /* Short, non-blocking-ish lock: the node task only ever holds this
         * for a couple of instructions, so a small timeout here cannot
         * stall the WiFi task in practice. On the rare miss, the sweep
         * simply continues to the next channel/sweep and retries. */
        if (!s_node_lock || xSemaphoreTake(s_node_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
        bool match = (ack.nonce == s_node_nonce);
        /* Observability: cheap (one log call, no NVS/flash, no send), so
         * safe to leave on the receive-callback path. Confirms the node's
         * receiver heard an ack at all, and separates "no ack heard" from
         * "ack heard but for someone else's/an earlier request" -- logging
         * both nonces on a mismatch is what actually distinguishes those
         * two cases from the console instead of just saying "MISMATCH". */
        ESP_LOGI(TAG, "PAIR_ACK from " MACSTR " nonce=%s (ours=0x%08" PRIx32 " theirs=0x%08" PRIx32 ")",
                 MAC2STR(src), match ? "match" : "MISMATCH", s_node_nonce, ack.nonce);
        if (match) {
            s_node_ack = ack;
            memcpy(s_node_ack_mac, src, 6);
        }
        xSemaphoreGive(s_node_lock);

        if (match) xSemaphoreGive(s_node_ack_sem);
        return;
    }

    /* Anything else (READING, PING, PONG, garbage) is not a pairing frame. */
}

/* ---------------- Node-side channel resync ---------------- */

esp_err_t pairing_node_resync_channel(void)
{
    uint8_t hub_mac[6], hub_lmk[SWARM_LMK_LEN], hub_ch;
    if (!swarm_store_hub(hub_mac, hub_lmk, &hub_ch)) return ESP_ERR_INVALID_STATE;

    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_err_t cherr = espnow_link_set_channel(ch);
        if (cherr != ESP_OK) {
            ESP_LOGW(TAG, "resync: set_channel(%u) failed: %s", ch, esp_err_to_name(cherr));
            continue;
        }

        uint8_t ping[2] = { SWARM_PROTO_VERSION, SWARM_MSG_PING };
        esp_err_t serr = espnow_link_send(hub_mac, ping, sizeof(ping));
        if (serr == ESP_OK) {
            esp_err_t store_err = swarm_store_set_channel(ch);
            if (store_err != ESP_OK) {
                ESP_LOGW(TAG, "resync: swarm_store_set_channel(%u) failed: %s",
                         ch, esp_err_to_name(store_err));
            }
            ESP_LOGI(TAG, "resync: hub reachable on channel %u", ch);
            return ESP_OK;
        }
    }

    /* Nothing answered on any of the 13 channels: don't leave the radio
     * parked wherever the sweep happened to stop (channel 13). Restore the
     * last known-good channel so this node keeps listening where the hub
     * was last confirmed, rather than somewhere arbitrary, until the next
     * resync attempt. */
    esp_err_t restore_err = espnow_link_set_channel(hub_ch);
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "resync: failed to restore last-known channel %u: %s",
                 hub_ch, esp_err_to_name(restore_err));
    } else {
        ESP_LOGI(TAG, "resync: restored last-known channel %u after failed sweep", hub_ch);
    }

    ESP_LOGW(TAG, "resync: hub not reachable on any channel");
    return ESP_ERR_NOT_FOUND;
}
