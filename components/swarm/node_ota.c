/* Hub-side node OTA session. See node_ota.h for the public contract and
 * threading rules, and the plan (Task 4) for the full behavioural spec.
 *
 * Image source: the hub's OWN running partition (esp_ota_get_running_partition()
 * + esp_partition_read()) -- a node always lands on exactly the firmware the
 * hub itself runs. The true image length is NOT the partition size (a
 * partition is always somewhat larger than the image it holds, and the tail
 * is unwritten/stale flash from whatever was there before): it has to come
 * from parsing the image, which is exactly what esp_image_get_metadata()
 * (bootloader_support, esp_image_format.h) does -- verified against this
 * repo's installed IDF (5.3.2, ~/esp/esp-idf) before writing any of this.
 * esp_image_metadata_t.image_len is documented there as "Length of image on
 * flash, in bytes", computed by walking the image header + every segment
 * header, which is precisely the "parse the header and segment headers"
 * requirement from the plan -- reusing IDF's own parser instead of
 * reimplementing esp_image_header_t/esp_image_segment_header_t walking here
 * means this also inherits IDF's own checksum/magic validation for free. If
 * esp_image_get_metadata() fails, the session fails immediately with
 * NODE_OTA_ERR_IMAGE_LEN rather than ever streaming the full ~1.6MB
 * partition guessing at a length. */
#include "node_ota.h"
#include "swarm_store.h"
#include "espnow_link.h"

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "node_ota";

/* Go-back-N tuning, per the plan. */
#define NODE_OTA_STATUS_STALL_MS      5000     /* how long OTA_STATUS silence must persist before
                                                  * counting as one stall -- no longer rewinds
                                                  * sent_offset by itself, see the stall-handling
                                                  * comment in the main loop below (fix, M5c
                                                  * hardware round 1) */
#define NODE_OTA_MAX_STALLS           8        /* abort after this many consecutive stalls (was 3;
                                                  * raised because OTA_STATUS is now a broadcast --
                                                  * see swarm_frame.h -- so occasionally losing one
                                                  * is now routine, not evidence the node/link is
                                                  * actually gone) */
#define NODE_OTA_TOTAL_TIMEOUT_US     ((int64_t)10 * 60 * 1000000)  /* 10 minutes */
#define NODE_OTA_CHUNK_YIELD_MS       2        /* pace: yield between chunks */
#define NODE_OTA_SEND_BACKOFF_MIN_MS  50
#define NODE_OTA_SEND_BACKOFF_MAX_MS  1000
#define NODE_OTA_BEGIN_MAX_ATTEMPTS   5
#define NODE_OTA_HASH_CHUNK           512

/* s_mutex guards ONLY this struct (RAM only, no I/O under it -- same
 * discipline as swarm_store's s_mutex, see its file header). node_ota_task()
 * is the sole writer of total_len/sent_offset/state/err/started_s outside of
 * session setup; node_ota_handle_status() (the ESP-NOW receive callback)
 * only ever writes last_status/has_status, and uses a NON-BLOCKING mutex
 * attempt so it can never stall the WiFi driver task even transiently (see
 * node_ota_handle_status() below). */
typedef struct {
    node_ota_progress_t pub;   /* mirrors node_ota_progress_t exactly, field for field */

    uint32_t             session_id;  /* fresh esp_random() value per node_ota_start() call,
                                        * set alongside pub.total_len below (same critical
                                        * section) before OTA_BEGIN is ever sent; carried in
                                        * that BEGIN and checked against every OTA_STATUS in
                                        * node_ota_handle_status() below -- see swarm_frame.h's
                                        * swarm_ota_begin_t comment for why (M5c hardware
                                        * round 1 fix). */
    swarm_ota_status_t  last_status;
    bool                has_status;   /* true = last_status is unread by the task yet */

    volatile bool       abort_requested;
} ota_session_t;

static SemaphoreHandle_t s_mutex;
static ota_session_t     s_session;
static TaskHandle_t      s_task;

static bool ensure_state(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

void node_ota_progress(node_ota_progress_t *out)
{
    if (!out) return;
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_session.pub;
    xSemaphoreGive(s_mutex);
}

/* ESP-NOW receive callback path (WiFi driver task) -- see node_ota.h and
 * the project-wide rule (PlanV1 Global Constraints): must never send, write
 * NVS, or block on flash. A non-blocking mutex attempt (timeout 0) is used
 * here specifically so this function can NEVER stall the caller even for
 * the short, bounded duration node_ota_task() might otherwise briefly hold
 * s_mutex for -- dropping this one status update on the rare occasion the
 * mutex is momentarily held is strictly preferable to ever blocking the
 * radio driver task, and is harmless: the node re-sends status every 64
 * chunks and at completion, and the sender's own 5s-no-status stall timer
 * independently covers a status that never arrives at all. */
void node_ota_handle_status(const uint8_t src[6], const swarm_ota_status_t *st)
{
    if (!src || !st || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;
    /* Session identity check (M5c hardware round 1 fix, required now that
     * OTA_STATUS is a plaintext broadcast -- see swarm_frame.h): src alone
     * is no longer enough, since anyone in radio range can broadcast a
     * frame claiming to be from a paired node's MAC (hub_rx_cb()'s
     * is_paired_node() already screens for that at the dispatch level, this
     * is the second, session-specific gate). Requiring session_id to match
     * the value THIS session generated and sent in its own OTA_BEGIN means a
     * status left over from an aborted or superseded session -- e.g. a
     * stale broadcast still in flight after the hub already gave up and
     * started a fresh push to the same node -- can never be credited to the
     * new one. */
    if (s_session.pub.active && memcmp(s_session.pub.mac, src, 6) == 0
        && st->session_id == s_session.session_id) {
        s_session.last_status = *st;
        /* Clamp (M5c hardware round 1 fix): a reported next_offset must
         * never be trusted past total_len -- a malformed, stale, or
         * (broadcast, so unauthenticated-by-encryption) malicious frame
         * claiming an offset beyond the image's real length must not be
         * allowed to make the go-back-N sender below believe it has
         * finished sending when it hasn't, or read/send past the end of
         * the source partition. */
        if (s_session.last_status.next_offset > s_session.pub.total_len) {
            s_session.last_status.next_offset = s_session.pub.total_len;
        }
        s_session.has_status = true;
    }
    xSemaphoreGive(s_mutex);
}

static bool abort_was_requested(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool r = s_session.abort_requested;
    xSemaphoreGive(s_mutex);
    return r;
}

esp_err_t node_ota_abort(void)
{
    if (!ensure_state()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = s_session.pub.active;
    if (active) s_session.abort_requested = true;
    xSemaphoreGive(s_mutex);
    return active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* Ends the session: updates the published state/err, sends a best-effort
 * OTA_ABORT to the node when `notify_node` (never for a clean DONE, since
 * the node isn't waiting on anything further there), clears `active` so a
 * new node_ota_start() can proceed, and returns. Does NOT delete the
 * calling task -- node_ota_task() does that itself right after calling
 * this, so any final logging there still has a live task context. */
static void finish_session(const uint8_t mac[6], uint8_t state, uint8_t err, bool notify_node, uint8_t reason)
{
    if (notify_node) {
        swarm_ota_abort_t ab = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_ABORT, .reason = reason };
        uint8_t buf[sizeof(ab)];
        size_t n = swarm_encode_ota_abort(&ab, buf, sizeof(buf));
        if (n) {
            esp_err_t err2 = espnow_link_send(mac, buf, n);
            ESP_LOGI(TAG, "OTA_ABORT -> " MACSTR " (reason=%u): %s", MAC2STR(mac), reason, esp_err_to_name(err2));
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_session.pub.state = state;
    s_session.pub.err = err;
    s_session.pub.active = false;
    xSemaphoreGive(s_mutex);
}

/* Streaming SHA-256 of exactly `total_len` bytes of `part`, starting at
 * offset 0 -- never buffers the whole image (the plan's explicit
 * requirement), reading NODE_OTA_HASH_CHUNK bytes at a time instead. Yields
 * periodically so this pre-pass (a full sequential read of up to ~1.6MB
 * from flash) doesn't monopolise the CPU against forwarding/other tasks --
 * same pacing philosophy as the chunk-send loop below, just for the hash
 * pass specifically. */
static esp_err_t hash_partition(const esp_partition_t *part, uint32_t total_len, uint8_t digest_out[32])
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0 /* SHA-256, not SHA-224 */);

    uint8_t buf[NODE_OTA_HASH_CHUNK];
    uint32_t off = 0;
    esp_err_t err = ESP_OK;
    uint32_t since_yield = 0;
    while (off < total_len) {
        size_t take = total_len - off;
        if (take > sizeof(buf)) take = sizeof(buf);
        err = esp_partition_read(part, off, buf, take);
        if (err != ESP_OK) break;
        mbedtls_sha256_update(&sha, buf, take);
        off += (uint32_t)take;
        since_yield += (uint32_t)take;
        if (since_yield >= 16384) {
            vTaskDelay(1);
            since_yield = 0;
        }
    }
    if (err == ESP_OK) mbedtls_sha256_finish(&sha, digest_out);
    mbedtls_sha256_free(&sha);
    return err;
}

static esp_err_t send_ota_begin(const uint8_t mac[6], uint32_t total_len, const uint8_t sha256[32],
                                 uint32_t session_id)
{
    swarm_ota_begin_t begin = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_BEGIN,
                                 .session_id = session_id, .total_len = total_len };
    memcpy(begin.sha256, sha256, 32);
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strlcpy(begin.fw_version, desc->version, sizeof(begin.fw_version));
    } else {
        memset(begin.fw_version, 0, sizeof(begin.fw_version));
    }

    uint8_t buf[sizeof(begin)];
    size_t n = swarm_encode_ota_begin(&begin, buf, sizeof(buf));
    if (!n) return ESP_FAIL;

    /* OTA_BEGIN is load-bearing: without it the node never calls
     * esp_ota_begin() and every subsequent OTA_CHUNK is meaningless to it
     * (Task 5). A handful of retries here is cheap insurance against a
     * single transient radio failure aborting an entire session before it
     * even starts. `begin` (and therefore session_id) is built once, above,
     * OUTSIDE this loop -- every retry re-sends the exact same frame, which
     * is what lets node_ota_recv.c's handle_begin() recognise a retry that
     * DID arrive the first time as an idempotent retransmission (same
     * session_id) instead of a conflicting second session (M5c hardware
     * round 1 fix: an ESP-NOW send failure here only means this hub's radio
     * didn't get a clean callback, not that the node never received it). */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= NODE_OTA_BEGIN_MAX_ATTEMPTS; attempt++) {
        err = espnow_link_send(mac, buf, n);
        ESP_LOGI(TAG, "OTA_BEGIN -> " MACSTR " attempt %d/%d (session=%" PRIu32 " total_len=%" PRIu32 " fw=%s): %s",
                 MAC2STR(mac), attempt, NODE_OTA_BEGIN_MAX_ATTEMPTS, session_id, total_len, begin.fw_version,
                 esp_err_to_name(err));
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

static void node_ota_task(void *arg)
{
    (void)arg;
    uint8_t mac[6];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(mac, s_session.pub.mac, 6);
    xSemaphoreGive(s_mutex);

    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part) {
        ESP_LOGE(TAG, "node OTA for " MACSTR ": esp_ota_get_running_partition() failed", MAC2STR(mac));
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_NO_PARTITION, false, 0);
        goto done;
    }

    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t meta;
    esp_err_t ierr = esp_image_get_metadata(&pos, &meta);
    if (ierr != ESP_OK || meta.image_len == 0 || meta.image_len > part->size) {
        /* Fail clearly rather than ever streaming the whole partition --
         * the plan's explicit requirement when the true length can't be
         * determined. */
        ESP_LOGE(TAG, "node OTA for " MACSTR ": cannot determine running image length (%s, "
                      "image_len=%" PRIu32 ", partition size=%" PRIu32 ")",
                 MAC2STR(mac), esp_err_to_name(ierr), meta.image_len, part->size);
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_IMAGE_LEN, false, 0);
        goto done;
    }
    uint32_t total_len = meta.image_len;

    /* Abort is only checked inside the main send loop below (it needs
     * OTA_BEGIN to have gone out first for an OTA_ABORT notification to
     * mean anything to the node) -- but the hash pass over up to ~1.6MB is
     * the one place before that loop slow enough to make a caller's
     * node_ota_abort() feel unresponsive if not also honoured here. No
     * OTA_ABORT is sent in either of these two early-exit cases: the node
     * was never told a session was starting, so there is nothing on its
     * side to abort. */
    if (abort_was_requested()) {
        ESP_LOGW(TAG, "node OTA for " MACSTR ": abort requested before hashing began", MAC2STR(mac));
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_ABORTED, false, 0);
        goto done;
    }

    uint8_t digest[32];
    if (hash_partition(part, total_len, digest) != ESP_OK) {
        ESP_LOGE(TAG, "node OTA for " MACSTR ": failed reading the running partition while hashing", MAC2STR(mac));
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_READ, false, 0);
        goto done;
    }

    /* session_id (M5c hardware round 1 fix): one fresh esp_random() value
     * for this entire node_ota_start() call, stored alongside total_len in
     * the same critical section so node_ota_handle_status() (which reads
     * both under s_mutex) never observes one without the other. See
     * swarm_frame.h's swarm_ota_begin_t comment for the full rationale. */
    uint32_t session_id = esp_random();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_session.pub.total_len = total_len;
    s_session.session_id = session_id;
    xSemaphoreGive(s_mutex);

    if (abort_was_requested()) {
        ESP_LOGW(TAG, "node OTA for " MACSTR ": abort requested before OTA_BEGIN was sent", MAC2STR(mac));
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_ABORTED, false, 0);
        goto done;
    }

    if (send_ota_begin(mac, total_len, digest, session_id) != ESP_OK) {
        ESP_LOGE(TAG, "node OTA for " MACSTR ": OTA_BEGIN never got out after %d attempts, giving up",
                 MAC2STR(mac), NODE_OTA_BEGIN_MAX_ATTEMPTS);
        finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_BEGIN_SEND, false, 0);
        goto done;
    }

    {
        int64_t started_us = esp_timer_get_time();
        int64_t last_status_us = started_us;
        int consecutive_stalls = 0;
        uint32_t send_backoff_ms = 0;

        for (;;) {
            if (abort_was_requested()) {
                ESP_LOGW(TAG, "node OTA for " MACSTR ": abort requested", MAC2STR(mac));
                finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_ABORTED, true, NODE_OTA_ERR_ABORTED);
                break;
            }

            if (esp_timer_get_time() - started_us > NODE_OTA_TOTAL_TIMEOUT_US) {
                ESP_LOGW(TAG, "node OTA for " MACSTR ": 10 minute total timeout", MAC2STR(mac));
                finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_TIMEOUT, true, NODE_OTA_ERR_TIMEOUT);
                break;
            }

            /* Drain the latest status, if any -- see node_ota_handle_status().
             * "Latest wins" is intentional: an intermediate status superseded
             * by a newer one before this task got to look carries no extra
             * information (next_offset is monotonic in normal operation). */
            swarm_ota_status_t st = {0};
            bool got_status = false;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_session.has_status) {
                st = s_session.last_status;
                s_session.has_status = false;
                got_status = true;
            }
            xSemaphoreGive(s_mutex);

            if (got_status) {
                last_status_us = esp_timer_get_time();
                consecutive_stalls = 0;

                if (st.state == OTA_ST_DONE) {
                    ESP_LOGI(TAG, "node OTA for " MACSTR ": node reports DONE", MAC2STR(mac));
                    finish_session(mac, OTA_ST_DONE, NODE_OTA_ERR_NONE, false, 0);
                    break;
                }
                if (st.state == OTA_ST_FAILED) {
                    ESP_LOGW(TAG, "node OTA for " MACSTR ": node reports FAILED (err=%u)",
                             MAC2STR(mac), st.err);
                    finish_session(mac, OTA_ST_FAILED, st.err, false, 0);
                    break;
                }

                /* RECEIVING: reconcile go-back-N offsets. A rewind only ever
                 * happens here, driven by the node's OWN reported next_offset
                 * actually being behind sent_offset -- this is now the ONLY
                 * place sent_offset can move backwards (M5c hardware round 1
                 * fix; see the stall branch below for why mere silence no
                 * longer does this too). */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (st.next_offset > s_session.pub.acked_offset) s_session.pub.acked_offset = st.next_offset;
                if (st.next_offset < s_session.pub.sent_offset) {
                    ESP_LOGI(TAG, "node OTA for " MACSTR ": rewind %" PRIu32 " -> %" PRIu32 " (node behind)",
                             MAC2STR(mac), s_session.pub.sent_offset, st.next_offset);
                    s_session.pub.sent_offset = st.next_offset;
                }
                xSemaphoreGive(s_mutex);
            } else if (esp_timer_get_time() - last_status_us > NODE_OTA_STATUS_STALL_MS * 1000) {
                /* Tolerate lost status (fix, M5c hardware round 1): the
                 * first real hardware OTA showed roughly a third of the
                 * node's OTA_STATUS frames failing to arrive here even
                 * while chunks kept sending and landing successfully.
                 * Rewinding sent_offset back to acked_offset on every such
                 * silence (as this branch used to) punished that as if the
                 * node had actually fallen behind, discarding perfectly
                 * good in-flight progress purely because the *report* of
                 * progress went missing -- and burned through the stall
                 * budget doing it, on a transfer that was otherwise healthy.
                 * A status that never arrives says nothing about where the
                 * node's next_offset actually is; only a status that DOES
                 * arrive and reports a lower offset (the branch above) is
                 * trustworthy evidence of that, so this branch no longer
                 * touches sent_offset at all -- it keeps streaming from
                 * wherever it already was. Still bounded: NODE_OTA_MAX_STALLS
                 * consecutive misses (now 8, up from 3, since an occasional
                 * lost broadcast is routine, not a sign of a dead link) and
                 * the unconditional NODE_OTA_TOTAL_TIMEOUT_US cap above both
                 * still give up on a genuinely dead node/link rather than
                 * streaming forever. */
                consecutive_stalls++;
                last_status_us = esp_timer_get_time();
                ESP_LOGW(TAG, "node OTA for " MACSTR ": stall #%d (no status in %dms), still "
                              "streaming from sent_offset=%" PRIu32 " (not rewound)",
                         MAC2STR(mac), consecutive_stalls, NODE_OTA_STATUS_STALL_MS,
                         s_session.pub.sent_offset);
                if (consecutive_stalls >= NODE_OTA_MAX_STALLS) {
                    ESP_LOGE(TAG, "node OTA for " MACSTR ": %d consecutive stalls, aborting",
                             MAC2STR(mac), consecutive_stalls);
                    finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_STALL, true, NODE_OTA_ERR_STALL);
                    break;
                }
            }

            uint32_t sent_offset, total;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            sent_offset = s_session.pub.sent_offset;
            total = s_session.pub.total_len;
            xSemaphoreGive(s_mutex);

            if (sent_offset >= total) {
                /* Everything sent at least once; idle briefly waiting for the
                 * node's final OTA_STATUS{DONE} (or a stall/timeout/abort,
                 * all still checked every iteration above). */
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            swarm_ota_chunk_t chunk = { .version = SWARM_PROTO_VERSION, .type = SWARM_MSG_OTA_CHUNK, .offset = sent_offset };
            uint32_t remaining = total - sent_offset;
            chunk.len = (uint16_t)(remaining > SWARM_OTA_CHUNK_DATA ? SWARM_OTA_CHUNK_DATA : remaining);

            esp_err_t rerr = esp_partition_read(part, sent_offset, chunk.data, chunk.len);
            if (rerr != ESP_OK) {
                ESP_LOGE(TAG, "node OTA for " MACSTR ": esp_partition_read failed at offset %" PRIu32 ": %s",
                         MAC2STR(mac), sent_offset, esp_err_to_name(rerr));
                finish_session(mac, OTA_ST_FAILED, NODE_OTA_ERR_READ, true, NODE_OTA_ERR_READ);
                break;
            }

            uint8_t cbuf[sizeof(chunk)];
            size_t cn = swarm_encode_ota_chunk(&chunk, cbuf, sizeof(cbuf));
            esp_err_t serr = cn ? espnow_link_send(mac, cbuf, cn) : ESP_FAIL;

            if (serr == ESP_OK) {
                send_backoff_ms = 0;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_session.pub.sent_offset = sent_offset + chunk.len;
                xSemaphoreGive(s_mutex);
                /* Pace the sender: the node is writing flash and sharing one
                 * radio with pairing/forwarding traffic (M5b lesson). */
                vTaskDelay(pdMS_TO_TICKS(NODE_OTA_CHUNK_YIELD_MS));
            } else {
                /* Back off rather than spin (M5b lesson, same as
                 * forward_task()'s backlog backoff) -- do NOT advance
                 * sent_offset; the same chunk is retried after the delay. */
                send_backoff_ms = (send_backoff_ms == 0) ? NODE_OTA_SEND_BACKOFF_MIN_MS
                                                          : (send_backoff_ms < NODE_OTA_SEND_BACKOFF_MAX_MS
                                                             ? send_backoff_ms * 2 : NODE_OTA_SEND_BACKOFF_MAX_MS);
                ESP_LOGW(TAG, "node OTA for " MACSTR ": chunk send failed at offset %" PRIu32 " (%s), "
                              "backing off %" PRIu32 "ms",
                         MAC2STR(mac), sent_offset, esp_err_to_name(serr), send_backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(send_backoff_ms));
            }
        }
    }

done:
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t node_ota_start(const uint8_t node_mac[6])
{
    if (!node_mac) return ESP_ERR_INVALID_ARG;
    if (!ensure_state()) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_session.pub.active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Courtesy check, RAM-only (swarm_store's cache never touches flash for
     * reads -- see its own locking invariant comment): refuse a target that
     * isn't actually a paired node rather than letting the task discover
     * that only after spawning. */
    bool known = false;
    int n = swarm_store_node_count();
    for (int i = 0; i < n; i++) {
        uint8_t stored[6];
        if (swarm_store_node_at(i, stored, NULL) && memcmp(stored, node_mac, 6) == 0) { known = true; break; }
    }
    if (!known) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_session, 0, sizeof(s_session));
    s_session.pub.active = true;
    memcpy(s_session.pub.mac, node_mac, 6);
    s_session.pub.state = OTA_ST_RECEIVING;
    s_session.pub.started_s = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreGive(s_mutex);

    BaseType_t ok = xTaskCreate(node_ota_task, "node_ota", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "node_ota_start: xTaskCreate failed for " MACSTR, MAC2STR(node_mac));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_session.pub.active = false;
        s_session.pub.state = OTA_ST_FAILED;
        s_session.pub.err = NODE_OTA_ERR_NO_MEM;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "node OTA session started for " MACSTR, MAC2STR(node_mac));
    return ESP_OK;
}
