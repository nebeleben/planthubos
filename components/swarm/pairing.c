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
#include "esp_wifi.h"
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

/* ---------------- Hub side: PONG responder (liveness) ---------------- */

/* Answering a PING is unrelated to adoption (no window gating, no
 * idempotent-MAC bookkeeping, no NVS write at all) and must be available
 * from the moment this device starts acting as a hub, not only once an
 * operator has opened a pairing window -- unlike s_adopt_queue/hub_task
 * above, so it gets its own small queue/task rather than sharing those. */
typedef struct {
    uint8_t  mac[6];   /* the pinging node, for logging only */
    uint32_t nonce;
} pending_pong_t;

#define PONG_QUEUE_LEN 4

static QueueHandle_t s_pong_queue;
static TaskHandle_t s_pong_task;

static void pong_task(void *arg)
{
    (void)arg;
    pending_pong_t item;
    for (;;) {
        if (xQueueReceive(s_pong_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        swarm_pong_t pong = {
            .version = SWARM_PROTO_VERSION,
            .type = SWARM_MSG_PONG,
            .nonce = item.nonce,
        };
        uint8_t buf[sizeof(pong)];
        size_t n = swarm_encode_pong(&pong, buf, sizeof(buf));
        if (n == 0) {
            ESP_LOGE(TAG, "PONG for " MACSTR " (nonce=0x%08" PRIx32 "): failed to encode",
                     MAC2STR(item.mac), item.nonce);
            continue;
        }

        /* BROADCAST, not unicast -- same reason as PAIR_ACK (see hub_task()
         * above): a node mid-resync may be probing a channel its AP
         * association has no bearing on, and is never associated to begin
         * with, so a unicast reply from this AP-associated hub is exactly
         * as unreliable as a unicast PAIR_ACK would be. The requesting
         * node matches this reply to its own PING purely by nonce, same
         * pattern as PAIR_ACK/PAIR_REQ. */
        esp_err_t err = espnow_link_broadcast(buf, n);
        ESP_LOGI(TAG, "PONG -> broadcast (ping from " MACSTR ", nonce=0x%08" PRIx32 ") result=%s",
                 MAC2STR(item.mac), item.nonce, esp_err_to_name(err));
    }
}

/* Idempotent; safe to call more than once. Must be called before any PING
 * can be answered -- swarm_start_main() calls this right after
 * espnow_link_init() so the responder exists from hub boot onward,
 * independent of whether/when a pairing window is ever opened. */
esp_err_t pairing_hub_init(void)
{
    if (s_pong_task) return ESP_OK;
    if (!s_pong_queue) s_pong_queue = xQueueCreate(PONG_QUEUE_LEN, sizeof(pending_pong_t));
    if (!s_pong_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(pong_task, "pairing_pong", 3072, NULL, 5, &s_pong_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "pairing_hub_init: xTaskCreate(pairing_pong) failed");
        s_pong_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
             * unicast, plaintext"; do NOT "harden" this by adding the peer
             * encrypted before sending, that breaks pairing outright (a
             * peer without the LMK cannot decrypt a frame encrypted with
             * it). Add the peer unencrypted first regardless of how the
             * ack itself is transmitted below, since the encrypted upgrade
             * further down is still what the steady-state unicast DATA
             * frames use once this node is paired. */
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

            /* Country inheritance (PlanV1 3.3): the hub is associated to
             * an AP, so ITS radio may legitimately end up on a country
             * different from (or more specific than) whatever this
             * firmware was compiled with -- esp_wifi_get_country() reads
             * back whatever is actually in effect right now. This does
             * NOT change how the hub configures its own radio (that stays
             * exactly as espnow_link_init() already sets it, compile-time
             * CONFIG_PLANTHUB_WIFI_COUNTRY, manual policy, unchanged by
             * this task); only what gets reported to the node is
             * "learned". A read failure is not fatal to pairing -- an
             * empty country just means the node keeps its own compile-time
             * default, exactly as if this were a pre-v2 hub. */
            wifi_country_t country;
            if (esp_wifi_get_country(&country) == ESP_OK) {
                memcpy(ack.country, country.cc, sizeof(ack.country));
                /* country.cc is NOT a NUL-terminated 2-letter code: IDF
                 * defines it as a 3-byte array whose third octet is the
                 * 802.11d "environment" character ('O'=outdoor, 'I'=indoor,
                 * 'X'=non-country-specific, ' '=either) -- e.g. Switzerland
                 * reads back as {'C','H','O'} or {'C','H',' '}, never
                 * {'C','H','\0'}. Force the NUL here so this frame's
                 * ack.country is always a proper 2-char C string: the node
                 * passes it straight to esp_wifi_set_country_code() as a
                 * const char *, %s's it into logs, and persists it, so a raw
                 * copy is an out-of-bounds read past this 3-byte buffer on
                 * every one of those uses, and would additionally persist
                 * "CH " instead of "CH". Do NOT "restore" the plain memcpy
                 * above to also cover this byte -- that's the bug. */
                ack.country[2] = '\0';
            } else {
                ESP_LOGW(TAG, "adopt " MACSTR ": esp_wifi_get_country failed; PAIR_ACK will "
                              "carry no country (node keeps its compile-time default)",
                         MAC2STR(item.mac));
                memset(ack.country, 0, sizeof(ack.country));
            }

            uint8_t buf[sizeof(ack)];
            size_t n = swarm_encode_pair_ack(&ack, buf, sizeof(buf));
            if (n == 0) {
                ESP_LOGE(TAG, "adopt " MACSTR ": failed to encode PAIR_ACK", MAC2STR(item.mac));
                break;
            }

            /* Send BEFORE persisting (unchanged reasoning: swarm_store_add_node()
             * below is a synchronous, multi-millisecond NVS/flash commit that
             * must not delay the ack past the node's sweep dwell).
             *
             * BROADCAST, not unicast, and this is the actual fix for the
             * one-sided pairing: on real hardware, a unicast PAIR_ACK got
             * ESP_FAIL (ESP_NOW_SEND_FAIL, i.e. no 802.11 MAC ack) on every
             * attempt even though both sides were confirmed on the same
             * channel and the plaintext/timing fixes above were already in
             * place. Root cause is the well-known ESP-NOW + WiFi mixed-mode
             * trap: the hub is an associated STA (its frames carry its AP's
             * BSSID) while the node is unassociated, and an unassociated
             * receiver's radio filters out -- and therefore never MAC-acks
             * -- a unicast frame tagged with a BSSID it isn't part of.
             * Broadcasting removes the MAC-ack dependency entirely (ESP-NOW
             * broadcasts are never acked, so there is no ESP_NOW_SEND_FAIL
             * to get). This is not a security regression: the ack was
             * already plaintext by design, the nonce it echoes was already
             * broadcast in clear as part of PAIR_REQ, and the node already
             * accepts an ack purely by nonce match (see pairing_handle_frame()) --
             * broadcasting the reply doesn't expose anything a unicast send
             * didn't already. */
            err = espnow_link_broadcast(buf, n);
            /* Observability: this handshake is otherwise invisible on the
             * console, which cost real diagnostic time tracking down a
             * one-sided pairing defect. Log every attempt's outcome, not
             * just failures. espnow_link_broadcast()'s result already
             * reflects "handed to the radio", not a MAC ack (broadcasts
             * have none) -- exactly the success criterion we want here. */
            ESP_LOGI(TAG, "PAIR_ACK -> broadcast (intended for " MACSTR ") channel=%u result=%s",
                     MAC2STR(item.mac), channel, esp_err_to_name(err));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "adopt " MACSTR ": PAIR_ACK broadcast failed: %s",
                         MAC2STR(item.mac), esp_err_to_name(err));
                break;
            }

            /* The node's sweep dwells long enough to have heard this
             * broadcast (or will on its next sweep, since the window stays
             * open), so it now has the key too -- safe to upgrade this
             * peer to encrypted on our side for the unicast DATA frames it
             * will send once paired. espnow_link_add_peer() calls
             * esp_now_mod_peer() in place when the peer already exists
             * (verified against this IDF's esp_now.h: esp_now_mod_peer()
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

/* Node-side resync state: separate from s_node_lock/s_node_ack_sem above on
 * purpose. Those belong to the initial pairing sweep (pairing_node_start()),
 * whose task has already exited (PAIR_OK/PAIR_FAILED, vTaskDelete()'d) by
 * the time resync ever runs; conflating the two would mean a resync's PONG
 * wait racing against state a dead task last touched. */
static SemaphoreHandle_t s_resync_lock;      /* guards s_resync_nonce / s_resync_waiting */
static SemaphoreHandle_t s_resync_pong_sem;  /* signalled by pairing_handle_frame on a matching PONG */
static volatile bool     s_resync_waiting;   /* true only while a resync attempt is between
                                                 sending its PING and giving up on this channel */
static uint32_t          s_resync_nonce;

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
    /* A channel outside the configured regulatory domain (see
     * CONFIG_PLANTHUB_WIFI_COUNTRY, applied in espnow_link_init()) fails
     * set_channel() on EVERY sweep, forever -- e.g. 12-13 under the IDF's
     * world-safe "01" default. Once, at WARN, is a useful signal that
     * something is regionally restricted; repeating it every ~6.5s for
     * the life of a search (up to 120s = ~18 sweeps) is just noise, so
     * only the first sweep logs at WARN and every later one drops to
     * DEBUG. */
    bool first_sweep = true;

    while (esp_timer_get_time() < deadline_us) {
        uint32_t nonce = esp_random();
        xSemaphoreTake(s_node_lock, portMAX_DELAY);
        s_node_nonce = nonce;
        xSemaphoreGive(s_node_lock);
        xSemaphoreTake(s_node_ack_sem, 0); /* drop any stale signal */

        for (uint8_t ch = 1; ch <= 13 && esp_timer_get_time() < deadline_us; ch++) {
            esp_err_t cherr = espnow_link_set_channel(ch);
            if (cherr != ESP_OK) {
                if (first_sweep) {
                    ESP_LOGW(TAG, "sweep: set_channel(%u) failed: %s", ch, esp_err_to_name(cherr));
                } else {
                    ESP_LOGD(TAG, "sweep: set_channel(%u) failed: %s", ch, esp_err_to_name(cherr));
                }
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

                /* Country inheritance (PlanV1 3.3): apply the hub's
                 * reported regulatory domain BEFORE persisting/using the
                 * channel it also sent -- so subsequent channel operations
                 * (the peer add and channel restore just below, and any
                 * future resync sweep) already run under the right
                 * domain, not the compile-time default, if the two
                 * differ. MANUAL policy (ieee80211d_enabled = false) is
                 * non-negotiable here, same as espnow_link_init(): this
                 * device never associates to any AP, and letting AUTO
                 * policy on disabled its transmitter outright on real
                 * hardware (see espnow_link.c). ack.country may be empty
                 * (hub's esp_wifi_get_country() failed, or a hypothetical
                 * pre-v2 hub) -- in that case, do nothing and keep
                 * whatever country this device already has (its own
                 * compile-time default, or one learned at an earlier
                 * pairing). */
                bool country_present = ack.country[0] != '\0';
                bool country_valid = country_present;
                if (country_present) {
                    /* Never trust the wire shape: on real hardware this
                     * field was found to arrive without the hub's intended
                     * NUL termination (see hub_task()'s comment on
                     * esp_wifi_get_country() -- its third octet is IDF's
                     * 802.11d environment character, not a terminator), and
                     * this path went unexercised until the very next
                     * pairing after that bug was introduced, since existing
                     * boards were migrated rather than re-paired. Validate
                     * before doing anything with it: require the two
                     * country-code characters to be alphanumeric, then force
                     * the NUL ourselves regardless of what the hub actually
                     * sent. A frame that fails validation is ignored outright
                     * -- this node keeps its compile-time default rather
                     * than applying/persisting garbage or reading past this
                     * 3-byte array. */
                    char c0 = ack.country[0], c1 = ack.country[1];
                    bool alnum0 = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9');
                    bool alnum1 = (c1 >= 'A' && c1 <= 'Z') || (c1 >= 'a' && c1 <= 'z') || (c1 >= '0' && c1 <= '9');
                    if (!alnum0 || !alnum1) {
                        ESP_LOGW(TAG, "PAIR_ACK country (bytes 0x%02x 0x%02x) is not alphanumeric; "
                                      "ignoring, keeping compile-time default",
                                 (unsigned)c0, (unsigned)c1);
                        country_valid = false;
                    } else {
                        ack.country[2] = '\0';
                    }
                }
                if (country_valid) {
                    esp_err_t cc_err = esp_wifi_set_country_code(ack.country, false);
                    if (cc_err != ESP_OK) {
                        ESP_LOGW(TAG, "esp_wifi_set_country_code(%s) failed: %s",
                                 ack.country, esp_err_to_name(cc_err));
                    } else {
                        esp_err_t cc_store_err = swarm_store_set_hub_country(ack.country);
                        if (cc_store_err != ESP_OK) {
                            ESP_LOGW(TAG, "swarm_store_set_hub_country(%s) failed: %s "
                                          "(country applied for this boot only, will not "
                                          "survive a reboot)",
                                     ack.country, esp_err_to_name(cc_store_err));
                        } else {
                            ESP_LOGI(TAG, "adopted hub country %s", ack.country);
                        }
                    }
                }

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
        first_sweep = false;
    }

    ESP_LOGW(TAG, "pairing timed out after %" PRIu32 "s", args.timeout_s);
    s_node_state = PAIR_FAILED;
    s_node_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- Node side: FORGET ---------------- */

/* Node side only. A FORGET frame means the hub no longer considers this
 * device paired (see swarm.c's forget_broadcast_task()): reacting means
 * removing the ESP-NOW peer, clearing swarm_store's hub record, setting
 * the pair-failed flag (so main.c's boot branch lands in the portal rather
 * than a fresh search) and restarting -- all NVS writes and a restart, none
 * of which pairing_handle_frame() (the WiFi-driver-task receive callback)
 * is allowed to do directly. So, same pattern as pong_task/hub_task above:
 * the callback only validates and hands off; this task does the actual
 * work. Queue depth 1 is enough -- once one FORGET is accepted the device
 * is about to restart, so there is nothing further to queue. */
typedef struct {
    uint8_t hub_mac[6];
} pending_forget_t;

static QueueHandle_t s_forget_queue;
static TaskHandle_t s_forget_task;

static void forget_task(void *arg)
{
    (void)arg;
    pending_forget_t item;
    for (;;) {
        if (xQueueReceive(s_forget_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGW(TAG, "FORGET accepted from hub " MACSTR "; clearing pairing and "
                      "restarting into the portal", MAC2STR(item.hub_mac));

        esp_err_t perr = espnow_link_remove_peer(item.hub_mac);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "FORGET: espnow_link_remove_peer failed: %s (restarting anyway)",
                     esp_err_to_name(perr));
        }
        swarm_store_clear_hub();
        swarm_store_set_pair_failed(true);
        esp_restart();
    }
}

/* Lazily created the first time this node ever accepts a real FORGET --
 * unlike pairing_hub_init()/ensure_hub_task() (called eagerly at hub boot),
 * there is no equivalent "node boot" hook that every node path already
 * calls, and a FORGET is rare enough that paying the one-time xTaskCreate()
 * cost right here, on the WiFi driver task, the first (and likely only)
 * time it is ever needed is preferable to adding a new public init call
 * that every node start path would have to remember. xTaskCreate() itself
 * is quick and does not touch NVS/flash, so this is still safe to do from
 * pairing_handle_frame(). */
static bool ensure_forget_task(void)
{
    if (s_forget_task) return true;
    if (!s_forget_queue) s_forget_queue = xQueueCreate(1, sizeof(pending_forget_t));
    if (!s_forget_queue) return false;
    BaseType_t ok = xTaskCreate(forget_task, "pairing_forget", 3072, NULL, 5, &s_forget_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ensure_forget_task: xTaskCreate(pairing_forget) failed");
        s_forget_task = NULL;
        return false;
    }
    return true;
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

        /* The hub now sends this as an ESP-NOW BROADCAST (see hub_task()'s
         * comment for why: a unicast ack from an AP-associated hub gets
         * silently filtered -- and never MAC-acked -- by an unassociated
         * node's radio). That requires no special handling here: `src` is
         * always the actual sender's MAC from the ESP-NOW receive metadata
         * (info->src_addr in espnow_link.c's on_recv, passed through
         * unchanged), regardless of whether the frame was addressed to us
         * unicast or to the broadcast address, and matching purely on
         * nonce (below) never assumed unicast addressing to begin with. */

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

    if (type == SWARM_MSG_PING) {
        /* Hub side only: a node (any node, not just one currently
         * resyncing -- there is no window/state gate here, deliberately,
         * since liveness must be answerable at any time) is asking
         * whether this hub is actually reachable at the application
         * layer. Decode, then hand off to pong_task() via a short,
         * non-blocking queue send -- this function must never call
         * espnow_link_broadcast() itself (see the threading note at the
         * top of this file). A missing/full queue (pairing_hub_init()
         * never called, or a burst of pings) just means this one PING
         * goes unanswered; the node will simply try the next channel. */
        swarm_ping_t ping;
        if (!swarm_decode_ping(data, (size_t)len, &ping)) return;

        pending_pong_t item;
        memcpy(item.mac, src, 6);
        item.nonce = ping.nonce;
        if (!s_pong_queue || xQueueSend(s_pong_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PING from " MACSTR ": no pong responder available, dropping",
                     MAC2STR(src));
        }
        return;
    }

    if (type == SWARM_MSG_PONG) {
        /* Node side only, and only meaningful while
         * pairing_node_resync_channel() is actively waiting on this exact
         * nonce -- s_resync_waiting is false the rest of the time
         * (including the entire initial pairing sweep above, which uses
         * its own separate state), so a PONG arriving outside a resync
         * attempt is simply ignored. */
        swarm_pong_t pong;
        if (!swarm_decode_pong(data, (size_t)len, &pong)) return;

        /* Nonce alone is not enough: PONG is a broadcast, so any device in
         * range during a resync attempt could reply with a guessed/replayed
         * nonce (the same accepted-risk trust tier as PAIR_ACK -- see above
         * -- but this check is cheap and narrows it further at no cost).
         * swarm_store_hub() is a short, bounded, allocation-free RAM-cache
         * read (no NVS touched), same reasoning already applied to every
         * other swarm_store access made from this receive-callback path, so
         * it's safe to call here. A PONG whose source isn't the hub this
         * node is actually paired to is never accepted, regardless of nonce. */
        uint8_t hub_mac[6];
        if (!swarm_store_hub(hub_mac, NULL, NULL) || memcmp(src, hub_mac, 6) != 0) return;

        if (!s_resync_lock || xSemaphoreTake(s_resync_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
        bool match = s_resync_waiting && (pong.nonce == s_resync_nonce);
        xSemaphoreGive(s_resync_lock);

        if (match) xSemaphoreGive(s_resync_pong_sem);
        return;
    }

    if (type == SWARM_MSG_FORGET) {
        /* Node side only (a hub never has a "stored hub" of its own, so
         * swarm_store_hub() below always fails there and this is a no-op
         * on that role). A forgotten node is no longer an encrypted
         * ESP-NOW peer on the hub's side, so this can only ever arrive as
         * a broadcast (see swarm_frame.h's swarm_forget_t comment) --
         * meaning `src` is whoever actually sent it, not necessarily
         * addressed to us specifically. Accept it only from the hub this
         * node believes it is paired to: swarm_store_hub() is the same
         * short, bounded, allocation-free RAM-cache read already used for
         * PONG's source check above (no NVS touched), so it's safe here
         * too. Note: every OTHER node currently paired to the SAME hub
         * will also see this broadcast and pass this same check -- see
         * swarm.c's swarm_broadcast_forget() for the accepted limitation
         * (forgetting one node currently forgets all of that hub's
         * nodes). */
        swarm_forget_t forget;
        if (!swarm_decode_forget(data, (size_t)len, &forget)) return;

        uint8_t hub_mac[6];
        if (!swarm_store_hub(hub_mac, NULL, NULL) || memcmp(src, hub_mac, 6) != 0) return;

        if (!ensure_forget_task()) {
            ESP_LOGE(TAG, "FORGET from " MACSTR ": forget task unavailable, cannot act on it",
                     MAC2STR(src));
            return;
        }
        pending_forget_t item;
        memcpy(item.hub_mac, hub_mac, 6);
        xQueueSend(s_forget_queue, &item, 0);  /* depth 1: already-queued one is enough, device is about to restart */
        return;
    }

    /* Anything else (READING, garbage) is not a pairing frame. */
}

/* ---------------- Node-side channel resync ---------------- */

/* How long to wait for a PONG after a PING was handed to the radio
 * successfully. Generous relative to the hub's turnaround (decode + a
 * non-blocking queue send + pong_task waking and broadcasting -- all of
 * which should complete in well under a millisecond of CPU time), to
 * absorb real-world scheduling jitter on both ends without materially
 * slowing a 13-channel sweep (worst case adds under 4s total). */
#define RESYNC_PONG_TIMEOUT_MS 300

esp_err_t pairing_node_resync_channel(void)
{
    uint8_t hub_mac[6], hub_lmk[SWARM_LMK_LEN], hub_ch;
    if (!swarm_store_hub(hub_mac, hub_lmk, &hub_ch)) return ESP_ERR_INVALID_STATE;

    if (!s_resync_lock) s_resync_lock = xSemaphoreCreateMutex();
    if (!s_resync_pong_sem) s_resync_pong_sem = xSemaphoreCreateBinary();
    if (!s_resync_lock || !s_resync_pong_sem) return ESP_ERR_NO_MEM;

    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_err_t cherr = espnow_link_set_channel(ch);
        if (cherr != ESP_OK) {
            ESP_LOGW(TAG, "resync: set_channel(%u) failed: %s", ch, esp_err_to_name(cherr));
            continue;
        }

        uint32_t nonce = esp_random();
        xSemaphoreTake(s_resync_lock, portMAX_DELAY);
        s_resync_nonce = nonce;
        s_resync_waiting = true;
        xSemaphoreGive(s_resync_lock);
        xSemaphoreTake(s_resync_pong_sem, 0);  /* drop any stale signal */

        swarm_ping_t ping = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_PING, .nonce = nonce };
        uint8_t buf[sizeof(ping)];
        size_t n = swarm_encode_ping(&ping, buf, sizeof(buf));
        esp_err_t serr = n ? espnow_link_send(hub_mac, buf, n) : ESP_FAIL;

        /* A delivered PING (serr == ESP_OK) only proves the hub's RADIO
         * MAC-acked the frame -- M5a trusted exactly that and was wrong to:
         * on real hardware, the hub's ESP-NOW layer can still silently
         * discard a frame after the MAC ack (unknown peer, undecryptable)
         * before its application layer ever sees it. So still wait for the
         * PONG even though the send "succeeded" -- that wait is the actual
         * liveness proof, not this esp_err_t. */
        bool got_pong = false;
        if (serr == ESP_OK) {
            got_pong = xSemaphoreTake(s_resync_pong_sem, pdMS_TO_TICKS(RESYNC_PONG_TIMEOUT_MS)) == pdTRUE;
        }

        xSemaphoreTake(s_resync_lock, portMAX_DELAY);
        s_resync_waiting = false;
        xSemaphoreGive(s_resync_lock);

        if (got_pong) {
            esp_err_t store_err = swarm_store_set_channel(ch);
            if (store_err != ESP_OK) {
                ESP_LOGW(TAG, "resync: swarm_store_set_channel(%u) failed: %s",
                         ch, esp_err_to_name(store_err));
            }
            ESP_LOGI(TAG, "resync: hub reachable on channel %u (pong confirmed)", ch);
            return ESP_OK;
        }
        if (serr == ESP_OK) {
            ESP_LOGD(TAG, "resync: channel %u PING was mac-acked but no PONG arrived "
                          "(radio reachable, application layer was not) -- trying the next channel", ch);
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
