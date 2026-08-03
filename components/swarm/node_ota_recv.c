/* Node-side OTA receiver. See node_ota_recv.h for the public contract and
 * threading rules, and the plan (Task 5) for the full behavioural spec.
 * Mirrors node_ota.c's hub-side session but is deliberately a separate
 * file/separate state (see node_ota_recv.h's header comment for why). */
#include "node_ota_recv.h"
#include "swarm_store.h"
#include "espnow_link.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "node_ota_recv";

#define NODE_OTA_RECV_QUEUE_LEN 8
#define NODE_OTA_RECV_STATUS_EVERY 64
#define NODE_OTA_RECV_RESTART_DELAY_MS 1500

/* Idle-timeout guard: if the hub that started a session crashes, reboots, or
 * otherwise drops out of radio contact mid-transfer, its (RAM-only) session
 * state vanishes without ever sending OTA_ABORT -- this node would otherwise
 * be left with s_session.active == true forever, rejecting every later
 * OTA_BEGIN (including a retry from a hub that comes back) with
 * NODE_OTA_RECV_ERR_ALREADY_ACTIVE until physically power-cycled. 30s is
 * generous next to the hub's own go-back-N stall timer (NODE_OTA_STATUS_STALL_MS
 * = 5000 in node_ota.c, tripping after NODE_OTA_MAX_STALLS = 3 consecutive
 * misses -- i.e. the hub itself gives up/rewinds well inside this window
 * during a merely-slow link) while still being short enough that an
 * operator retrying "shortly after" a hub crash doesn't have to wait long.
 * NODE_OTA_RECV_IDLE_CHECK_MS is just how often node_ota_recv_task() below
 * wakes on its own to re-check the above while otherwise idle waiting on
 * s_queue -- unrelated to the timeout duration itself. */
#define NODE_OTA_RECV_IDLE_TIMEOUT_MS 30000
#define NODE_OTA_RECV_IDLE_CHECK_MS   1000

/* Event handed from the ESP-NOW receive callback (node_rx_cb, WiFi driver
 * task) to node_ota_recv_task() below. Tagged union sized for the largest
 * member (swarm_ota_chunk_t, up to 208 bytes) -- see node_ota_recv.h for why
 * this hop exists at all (the callback must never touch flash/NVS/send). */
typedef struct {
    uint8_t type;   /* SWARM_MSG_OTA_BEGIN / _CHUNK / _ABORT */
    uint8_t src[6];
    union {
        swarm_ota_begin_t begin;
        swarm_ota_chunk_t chunk;
        swarm_ota_abort_t abort;
    } f;
} ota_recv_evt_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;

/* Task-owned only (node_ota_recv_task() is the sole reader/writer) -- no
 * mutex needed, same reasoning as swarm.c's s_buf: a single owner needs
 * none. The receive callback never touches this struct, only the queue
 * above. */
typedef struct {
    bool                    active;
    esp_ota_handle_t        handle;
    const esp_partition_t  *part;
    uint32_t                total_len;
    uint8_t                 sha256_expected[32];
    uint8_t                 hub_mac[6];       /* who started this session, for logging + a
                                                * defense-in-depth re-check on later frames */
    uint32_t                next_offset;
    uint32_t                chunks_since_status;
    mbedtls_sha256_context  sha_ctx;          /* only valid while active */
    int64_t                 last_activity_us; /* esp_timer_get_time() at the last accepted
                                                * OTA_BEGIN/OTA_CHUNK; only meaningful while
                                                * active -- see NODE_OTA_RECV_IDLE_TIMEOUT_MS */
} recv_session_t;

/* Cheap, bounded, allocation-free RAM-cache read (no NVS touched) -- same
 * reasoning already applied to every other swarm_store access made from
 * the ESP-NOW receive callback path elsewhere in this codebase (swarm.c's
 * is_paired_node(), pairing.c's PONG/FORGET handling). Safe to call
 * directly from node_ota_recv_handle_*(). */
static bool from_stored_hub(const uint8_t src[6])
{
    uint8_t hub_mac[6];
    return swarm_store_hub(hub_mac, NULL, NULL) && memcmp(src, hub_mac, 6) == 0;
}

static void enqueue(uint8_t type, const uint8_t src[6], const void *frame, size_t frame_len)
{
    if (!s_queue) return;
    ota_recv_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    memcpy(evt.src, src, 6);
    memcpy(&evt.f, frame, frame_len);
    /* Non-blocking: a full queue means the receiver task is behind (or not
     * yet running). Dropping is safe and self-healing -- see the header
     * comment: the periodic OTA_STATUS this node sends (or the hub's own
     * 5s stall timer) recovers from any gap. */
    xQueueSend(s_queue, &evt, 0);
}

void node_ota_recv_handle_begin(const uint8_t src[6], const swarm_ota_begin_t *begin)
{
    if (!src || !begin || !from_stored_hub(src)) return;
    enqueue(SWARM_MSG_OTA_BEGIN, src, begin, sizeof(*begin));
}

void node_ota_recv_handle_chunk(const uint8_t src[6], const swarm_ota_chunk_t *chunk)
{
    if (!src || !chunk || !from_stored_hub(src)) return;
    enqueue(SWARM_MSG_OTA_CHUNK, src, chunk, sizeof(*chunk));
}

void node_ota_recv_handle_abort(const uint8_t src[6], const swarm_ota_abort_t *ab)
{
    if (!src || !ab || !from_stored_hub(src)) return;
    enqueue(SWARM_MSG_OTA_ABORT, src, ab, sizeof(*ab));
}

/* Task-only (espnow_link_send() blocks -- never call this from the receive
 * callback). Best-effort: logs the outcome, does not retry. The hub's own
 * go-back-N sender independently recovers from a dropped/failed status via
 * its 5s stall timer, so a single failed send here is not fatal to the
 * session. */
static void send_status(const uint8_t dst[6], uint8_t state, uint8_t err, uint32_t next_offset)
{
    swarm_ota_status_t st = {
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_OTA_STATUS,
        .state = state,
        .err = err,
        .next_offset = next_offset,
    };
    uint8_t buf[sizeof(st)];
    size_t n = swarm_encode_ota_status(&st, buf, sizeof(buf));
    if (!n) return;
    esp_err_t serr = espnow_link_send(dst, buf, n);
    ESP_LOGI(TAG, "OTA_STATUS -> " MACSTR " state=%u err=%u next_offset=%" PRIu32 ": %s",
             MAC2STR(dst), state, err, next_offset, esp_err_to_name(serr));
}

static void handle_begin(recv_session_t *s, const uint8_t src[6], const swarm_ota_begin_t *begin)
{
    if (s->active) {
        ESP_LOGW(TAG, "OTA_BEGIN from " MACSTR " while a session is already active; rejecting",
                 MAC2STR(src));
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_ALREADY_ACTIVE, 0);
        return;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": no next update partition", MAC2STR(src));
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_NO_PARTITION, 0);
        return;
    }
    if (begin->total_len == 0 || begin->total_len > part->size) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": total_len %" PRIu32 " invalid for partition size %" PRIu32,
                 MAC2STR(src), begin->total_len, part->size);
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_TOO_LARGE, 0);
        return;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(part, begin->total_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": esp_ota_begin failed: %s", MAC2STR(src), esp_err_to_name(err));
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_BEGIN_FAILED, 0);
        return;
    }

    memset(s, 0, sizeof(*s));
    s->active = true;
    s->handle = handle;
    s->part = part;
    s->total_len = begin->total_len;
    memcpy(s->sha256_expected, begin->sha256, sizeof(s->sha256_expected));
    memcpy(s->hub_mac, src, 6);
    s->last_activity_us = esp_timer_get_time();
    mbedtls_sha256_init(&s->sha_ctx);
    mbedtls_sha256_starts(&s->sha_ctx, 0 /* SHA-256, not SHA-224 */);

    char fwv[sizeof(begin->fw_version) + 1];  /* +1: guarantee NUL even if the wire field isn't
                                                * (the encoder/caller "must" NUL-terminate per
                                                * swarm_frame.h, but this is attacker-controlled
                                                * plaintext-after-decrypt -- don't trust it blindly) */
    memcpy(fwv, begin->fw_version, sizeof(begin->fw_version));
    fwv[sizeof(begin->fw_version)] = '\0';    /* the guaranteed extra byte, not the last real one */
    ESP_LOGW(TAG, "OTA_BEGIN accepted from " MACSTR ": total_len=%" PRIu32 " fw=%s -> writing %s",
             MAC2STR(src), s->total_len, fwv, part->label);
    send_status(src, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, 0);
}

/* Finalizes a session whose next_offset has just reached total_len: order
 * matters and follows the plan literally -- esp_ota_end() (validates the
 * flashed image) first, THEN compare our own end-to-end streamed hash
 * against OTA_BEGIN's, THEN esp_ota_set_boot_partition(). Restarts on
 * success; never returns in that case. */
static void finalize_session(recv_session_t *s, const uint8_t src[6])
{
    esp_err_t eerr = esp_ota_end(s->handle);
    if (eerr != ESP_OK) {
        ESP_LOGE(TAG, "OTA finalize for " MACSTR ": esp_ota_end failed: %s", MAC2STR(src), esp_err_to_name(eerr));
        mbedtls_sha256_free(&s->sha_ctx);
        s->active = false;
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_END_FAILED, s->next_offset);
        return;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s->sha_ctx, digest);
    mbedtls_sha256_free(&s->sha_ctx);
    if (memcmp(digest, s->sha256_expected, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "OTA finalize for " MACSTR ": streamed sha256 does not match OTA_BEGIN's", MAC2STR(src));
        s->active = false;
        /* esp_ota_end() already succeeded and finalized the handle above --
         * there is no abortable handle left at this point (esp_ota_abort()
         * is only valid on a handle that hasn't been esp_ota_end()'d yet).
         * The only action needed is to NOT call esp_ota_set_boot_partition():
         * the bootloader keeps booting the CURRENT partition on the next
         * reboot regardless, the same "stay on current firmware" outcome
         * every other failure path here reaches via esp_ota_abort(). */
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_HASH_MISMATCH, s->next_offset);
        return;
    }

    esp_err_t berr = esp_ota_set_boot_partition(s->part);
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "OTA finalize for " MACSTR ": esp_ota_set_boot_partition failed: %s",
                 MAC2STR(src), esp_err_to_name(berr));
        s->active = false;
        send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_SET_BOOT_FAILED, s->next_offset);
        return;
    }

    s->active = false;
    send_status(src, OTA_ST_DONE, NODE_OTA_RECV_ERR_NONE, s->next_offset);
    ESP_LOGW(TAG, "node OTA complete (from " MACSTR "), restarting in %dms to boot %s",
             MAC2STR(src), NODE_OTA_RECV_RESTART_DELAY_MS, s->part->label);
    /* Delay so the DONE status frame above actually gets out over the radio
     * before this device restarts -- same reasoning as ota_post.c's
     * restart_cb, just via vTaskDelay() directly since (unlike that HTTP
     * handler) this task has no in-flight response it needs to return
     * first. */
    vTaskDelay(pdMS_TO_TICKS(NODE_OTA_RECV_RESTART_DELAY_MS));
    esp_restart();
}

static void handle_chunk(recv_session_t *s, const uint8_t src[6], const swarm_ota_chunk_t *chunk)
{
    if (!s->active) {
        ESP_LOGD(TAG, "OTA_CHUNK from " MACSTR " with no active session, ignoring", MAC2STR(src));
        return;
    }
    if (memcmp(src, s->hub_mac, 6) != 0) return;  /* defense in depth; callback already checked */

    if (chunk->offset == s->next_offset) {
        esp_err_t werr = esp_ota_write(s->handle, chunk->data, chunk->len);
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "OTA_CHUNK for " MACSTR " at offset %" PRIu32 ": esp_ota_write failed: %s",
                     MAC2STR(src), s->next_offset, esp_err_to_name(werr));
            esp_ota_abort(s->handle);
            mbedtls_sha256_free(&s->sha_ctx);
            s->active = false;
            send_status(src, OTA_ST_FAILED, NODE_OTA_RECV_ERR_WRITE_FAILED, s->next_offset);
            return;
        }
        mbedtls_sha256_update(&s->sha_ctx, chunk->data, chunk->len);
        s->next_offset += chunk->len;
        s->chunks_since_status++;
        s->last_activity_us = esp_timer_get_time();

        bool at_end = s->next_offset >= s->total_len;
        if (s->chunks_since_status >= NODE_OTA_RECV_STATUS_EVERY || at_end) {
            send_status(src, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, s->next_offset);
            s->chunks_since_status = 0;
        }
        if (at_end) finalize_session(s, src);   /* may esp_restart() and never return */
    } else if (chunk->offset < s->next_offset) {
        ESP_LOGD(TAG, "OTA_CHUNK offset %" PRIu32 " behind next_offset %" PRIu32 " (rewind replay), ignoring",
                 chunk->offset, s->next_offset);
    } else {
        ESP_LOGD(TAG, "OTA_CHUNK offset %" PRIu32 " ahead of next_offset %" PRIu32
                      ", ignoring (periodic status will pull the hub back)",
                 chunk->offset, s->next_offset);
    }
}

static void handle_abort(recv_session_t *s, const uint8_t src[6], const swarm_ota_abort_t *ab)
{
    if (!s->active) return;
    if (memcmp(src, s->hub_mac, 6) != 0) return;
    ESP_LOGW(TAG, "OTA_ABORT from hub " MACSTR " (reason=%u); aborting, staying on current firmware",
             MAC2STR(src), ab->reason);
    esp_ota_abort(s->handle);
    mbedtls_sha256_free(&s->sha_ctx);
    s->active = false;
}

/* Self-aborts `s` if it has been active for more than NODE_OTA_RECV_IDLE_TIMEOUT_MS
 * with no OTA_BEGIN/OTA_CHUNK accepted (see that constant's comment for why).
 * Only ever called from node_ota_recv_task() -- same single-task ownership as
 * every other recv_session_t mutation in this file, so this can never race a
 * chunk being processed: the task is either sitting in xQueueReceive() (this
 * check runs on its timeout branch) or already inside handle_begin()/
 * handle_chunk()/handle_abort() (this check hasn't been reached yet). No
 * OTA_STATUS is sent: the hub that started this session may well be gone
 * (crashed, rebooted, out of range) -- there is nothing on the other end to
 * notify, and a hub that does come back always starts over with a fresh
 * OTA_BEGIN, which this node is now free to accept immediately. */
static void check_idle_timeout(recv_session_t *s)
{
    if (!s->active) return;
    int64_t idle_us = esp_timer_get_time() - s->last_activity_us;
    if (idle_us < (int64_t)NODE_OTA_RECV_IDLE_TIMEOUT_MS * 1000) return;

    ESP_LOGW(TAG, "OTA session from " MACSTR " idle for >%dms with no OTA_BEGIN/OTA_CHUNK "
                  "accepted; self-aborting so a fresh OTA_BEGIN is accepted immediately",
             MAC2STR(s->hub_mac), NODE_OTA_RECV_IDLE_TIMEOUT_MS);
    esp_ota_abort(s->handle);
    mbedtls_sha256_free(&s->sha_ctx);
    s->active = false;
}

static void node_ota_recv_task(void *arg)
{
    (void)arg;
    static recv_session_t s_session;  /* task-owned only; zero-initialised (static) */

    for (;;) {
        ota_recv_evt_t evt;
        /* Bounded wait, not portMAX_DELAY: this task must wake on its own
         * even with no frames arriving at all, purely to re-check
         * check_idle_timeout() above -- an idle queue is exactly the
         * situation a hub-gone-silent leaves behind. */
        if (xQueueReceive(s_queue, &evt, pdMS_TO_TICKS(NODE_OTA_RECV_IDLE_CHECK_MS)) != pdTRUE) {
            check_idle_timeout(&s_session);
            continue;
        }

        if (evt.type == SWARM_MSG_OTA_BEGIN) {
            handle_begin(&s_session, evt.src, &evt.f.begin);
        } else if (evt.type == SWARM_MSG_OTA_CHUNK) {
            handle_chunk(&s_session, evt.src, &evt.f.chunk);
        } else if (evt.type == SWARM_MSG_OTA_ABORT) {
            handle_abort(&s_session, evt.src, &evt.f.abort);
        }
    }
}

esp_err_t node_ota_recv_init(void)
{
    if (s_task) return ESP_OK;
    if (!s_queue) s_queue = xQueueCreate(NODE_OTA_RECV_QUEUE_LEN, sizeof(ota_recv_evt_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(node_ota_recv_task, "node_ota_recv", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "node_ota_recv_init: xTaskCreate failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
