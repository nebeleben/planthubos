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

static SemaphoreHandle_t s_window_lock;   /* guards s_window_open / s_window_deadline_us */
static volatile bool s_window_open;
static int64_t s_window_deadline_us;

static QueueHandle_t s_adopt_queue;       /* depth 1: one pending adoption at a time */
static TaskHandle_t s_hub_task;

static void hub_task(void *arg);

static void ensure_hub_task(void)
{
    if (s_hub_task) return;
    if (!s_window_lock) s_window_lock = xSemaphoreCreateMutex();
    if (!s_adopt_queue) s_adopt_queue = xQueueCreate(1, sizeof(pending_adopt_t));
    xTaskCreate(hub_task, "pairing_hub", 4096, NULL, 5, &s_hub_task);
}

void pairing_open_window(uint32_t seconds)
{
    ensure_hub_task();

    xSemaphoreTake(s_window_lock, portMAX_DELAY);
    s_window_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_window_open = true;
    xSemaphoreGive(s_window_lock);

    ESP_LOGI(TAG, "pairing window open for %" PRIu32 "s", seconds);
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

/* Runs on its own task, never on the WiFi task: safe to touch NVS and to
 * call the blocking espnow_link_send(). */
static void hub_task(void *arg)
{
    (void)arg;
    pending_adopt_t item;

    for (;;) {
        if (xQueueReceive(s_adopt_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        uint8_t lmk[SWARM_LMK_LEN];
        esp_fill_random(lmk, sizeof(lmk));

        esp_err_t err = swarm_store_add_node(item.mac, lmk);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adopt " MACSTR ": swarm_store_add_node failed: %s",
                     MAC2STR(item.mac), esp_err_to_name(err));
            continue;
        }

        uint8_t channel = espnow_link_channel();
        err = espnow_link_add_peer(item.mac, lmk, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adopt " MACSTR ": espnow_link_add_peer failed: %s",
                     MAC2STR(item.mac), esp_err_to_name(err));
            continue;
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
            continue;
        }

        err = espnow_link_send(item.mac, buf, n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adopt " MACSTR ": PAIR_ACK send failed: %s",
                     MAC2STR(item.mac), esp_err_to_name(err));
            continue;
        }

        ESP_LOGI(TAG, "adopted node " MACSTR " on channel %u", MAC2STR(item.mac), channel);
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
    const TickType_t dwell_ticks = pdMS_TO_TICKS(300);

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
        if (!pairing_window_open()) return;

        swarm_pair_req_t req;
        if (!swarm_decode_pair_req(data, (size_t)len, &req)) return;

        pending_adopt_t item;
        memcpy(item.mac, src, 6);
        item.nonce = req.nonce;

        /* Close the window the instant we hand off an adoption so a
         * bystander can't join the same window (one node per window). If
         * the queue is already full (an adoption is already pending), drop
         * this one — the requester will simply retry on a future window. */
        if (s_adopt_queue && xQueueSend(s_adopt_queue, &item, 0) == pdTRUE) {
            xSemaphoreTake(s_window_lock, portMAX_DELAY);
            s_window_open = false;
            xSemaphoreGive(s_window_lock);
        }
        return;
    }

    if (type == SWARM_MSG_PAIR_ACK) {
        if (s_node_state != PAIR_SEARCHING) return;

        swarm_pair_ack_t ack;
        if (!swarm_decode_pair_ack(data, (size_t)len, &ack)) return;

        /* Short, non-blocking-ish lock: the node task only ever holds this
         * for a couple of instructions, so a small timeout here cannot
         * stall the WiFi task in practice. On the rare miss, the sweep
         * simply continues to the next channel/sweep and retries. */
        if (!s_node_lock || xSemaphoreTake(s_node_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
        bool match = (ack.nonce == s_node_nonce);
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

    ESP_LOGW(TAG, "resync: hub not reachable on any channel");
    return ESP_ERR_NOT_FOUND;
}
