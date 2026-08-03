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

/* Terminal DONE retransmission (fix, M5c hardware round 4, defect 1): the
 * single OTA_STATUS(state=OTA_ST_DONE) sent at the end of finalize_session()
 * below is this node's ONLY chance to tell the hub the transfer succeeded --
 * unlike every status during the transfer, there is no later chunk or status
 * to prompt a retry, because this node restarts shortly after. Round 4 hit
 * exactly that: that one frame was lost, the hub never learned the transfer
 * finished, and it reported the update FAILED even though it had actually
 * succeeded (verified sha256, booted app1, rollback cancelled). Resend the
 * same terminal status a handful of times, spaced out, instead of once --
 * losing one out of five is far less likely than losing the one-and-only
 * frame this used to be. Sized to fit comfortably inside
 * NODE_OTA_RECV_RESTART_DELAY_MS -- see the _Static_assert below -- per the
 * plan's explicit constraint not to lengthen the restart delay to fit this. */
#define NODE_OTA_RECV_DONE_RETRANSMITS   5
#define NODE_OTA_RECV_DONE_RETRANSMIT_MS 200
_Static_assert(NODE_OTA_RECV_DONE_RETRANSMITS * NODE_OTA_RECV_DONE_RETRANSMIT_MS <= NODE_OTA_RECV_RESTART_DELAY_MS,
               "terminal DONE retransmissions must fit inside the existing restart delay window");

/* Rate limit for the "ahead of next_offset" OTA_STATUS below (fix, M5c
 * hardware round 3 -- see handle_chunk()'s ahead-branch comment for the full
 * evidence/rationale). One per second is generous next to how often chunks
 * actually arrive (NODE_OTA_CHUNK_YIELD_MS = 2ms pacing in node_ota.c, i.e.
 * hundreds of chunks/second) while still being far more responsive than the
 * hub's multi-second stall patience (NODE_OTA_MAX_STALLS consecutive
 * non-advancing statuses in node_ota.c, plus its 10-minute total cap). */
#define NODE_OTA_RECV_AHEAD_STATUS_MIN_US ((int64_t)1000 * 1000)

/* Idle-timeout guard: if the hub that started a session crashes, reboots, or
 * otherwise drops out of radio contact mid-transfer, its (RAM-only) session
 * state vanishes without ever sending OTA_ABORT -- this node would otherwise
 * be left with s_session.active == true forever, rejecting every later
 * OTA_BEGIN (including a retry from a hub that comes back) with
 * NODE_OTA_RECV_ERR_ALREADY_ACTIVE until physically power-cycled. 30s is
 * generous next to the hub's own go-back-N stall handling (node_ota.c aborts
 * after NODE_OTA_MAX_STALLS consecutive OTA_STATUS reports whose next_offset
 * failed to advance -- with statuses roughly 1/s apart at worst once this
 * node's own ahead-branch rate limit above is in play -- i.e. the hub itself
 * gives up/rewinds well inside this window during a merely-slow link) while
 * still being short enough that an operator retrying "shortly after" a hub
 * crash doesn't have to wait long.
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
    uint32_t                session_id;       /* echoed from the accepted OTA_BEGIN; carried in
                                                * every OTA_STATUS this session sends -- see
                                                * swarm_frame.h's swarm_ota_begin_t comment */
    uint32_t                next_offset;
    uint32_t                chunks_since_status;
    mbedtls_sha256_context  sha_ctx;          /* only valid while active */
    int64_t                 last_activity_us; /* esp_timer_get_time() at the last accepted
                                                * OTA_BEGIN/OTA_CHUNK; only meaningful while
                                                * active -- see NODE_OTA_RECV_IDLE_TIMEOUT_MS */
    int64_t                 last_ahead_status_us; /* esp_timer_get_time() at the last
                                                * "ahead of next_offset" OTA_STATUS sent (0 =
                                                * none sent yet this session); see handle_chunk()'s
                                                * ahead-branch and NODE_OTA_RECV_AHEAD_STATUS_MIN_US */
    int64_t                 last_idle_status_us; /* esp_timer_get_time() at the last
                                                * state=OTA_ST_IDLE status sent for an OTA_CHUNK that
                                                * arrived with NO active session (0 = none sent yet).
                                                * Unlike the fields above it, this one is meaningful
                                                * precisely WHILE s->active is false -- handle_begin()'s
                                                * memset() on a fresh accept also clears it, which is
                                                * harmless: it just means the next idle chunk (after
                                                * this new session eventually ends) is answered
                                                * immediately rather than rate-limited from a stale
                                                * timestamp. See the !s->active branch of handle_chunk()
                                                * below (fix, M5c hardware round 4, defect 2a). */
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

/* Task-only (espnow_link_broadcast() blocks -- never call this from the
 * receive callback). Best-effort: logs the outcome, does not retry. The
 * hub's own go-back-N sender independently recovers from a dropped/failed
 * status via its stall handling, so a single failed send here is not fatal
 * to the session.
 *
 * BROADCAST, not unicast (fix, M5c hardware round 1): the first real
 * hardware OTA measured roughly a third of these failing to reach the hub's
 * application layer as a unicast send, exactly the 802.11 MAC-ack-vs-actual-
 * delivery gap PlanV1 8e already documents for PAIR_ACK/PONG -- see the full
 * writeup on swarm_ota_status_t in swarm_frame.h. `dst` is kept purely so
 * the log line below says which hub this status is for; it plays no part in
 * addressing the send itself any more. DO NOT change this back to
 * espnow_link_send(dst, ...) -- that is precisely the regression this fix
 * addresses (PlanV1 8e). */
static void send_status(const uint8_t dst[6], uint32_t session_id, uint8_t state, uint8_t err, uint32_t next_offset)
{
    swarm_ota_status_t st = {
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_OTA_STATUS,
        .session_id = session_id,
        .state = state,
        .err = err,
        .next_offset = next_offset,
    };
    uint8_t buf[sizeof(st)];
    size_t n = swarm_encode_ota_status(&st, buf, sizeof(buf));
    if (!n) return;
    esp_err_t serr = espnow_link_broadcast(buf, n);
    ESP_LOGI(TAG, "OTA_STATUS (broadcast, hub=" MACSTR ") session=%" PRIu32 " state=%u err=%u "
                  "next_offset=%" PRIu32 ": %s",
             MAC2STR(dst), session_id, state, err, next_offset, esp_err_to_name(serr));
}

static void handle_begin(recv_session_t *s, const uint8_t src[6], const swarm_ota_begin_t *begin)
{
    if (s->active) {
        /* Idempotent OTA_BEGIN (fix, M5c hardware round 1): node_ota.c's
         * send_ota_begin() retries up to NODE_OTA_BEGIN_MAX_ATTEMPTS times
         * whenever a send's outcome looks like a failure -- but an ESP-NOW
         * send failure reflects the local radio's own send-callback result,
         * not proof the peer never received the frame (the same class of
         * ack-vs-delivery ambiguity M5a hit for pairing). A retried BEGIN
         * that actually WAS received the first time then looked, from this
         * node's side, exactly like a second session starting mid-transfer:
         * rejected with FAILED(ALREADY_ACTIVE), which is both noisy (a
         * spurious error on an otherwise-healthy transfer) and misleading
         * (the hub has no way to tell that apart from a real conflict).
         *
         * Detect a genuine retransmission -- same hub, same session_id
         * (identical to what node_ota.c's send_ota_begin() built once,
         * before its retry loop, so every retry of the SAME attempt carries
         * the same value), same total_len/sha256 -- and just re-report
         * where this session already is instead of tearing anything down. */
        bool retransmit = memcmp(src, s->hub_mac, 6) == 0
                        && begin->session_id == s->session_id
                        && begin->total_len == s->total_len
                        && memcmp(begin->sha256, s->sha256_expected, sizeof(s->sha256_expected)) == 0;
        if (retransmit) {
            ESP_LOGI(TAG, "OTA_BEGIN from " MACSTR " session=%" PRIu32 " is a retransmission of "
                          "the active session; re-sending current status at next_offset=%" PRIu32,
                     MAC2STR(src), s->session_id, s->next_offset);
            send_status(src, s->session_id, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, s->next_offset);
            return;
        }

        /* A BEGIN with DIFFERENT parameters (different hub, session_id,
         * length, or image hash) while a session is active is REJECTED, not
         * used to restart the session mid-flight: this node may already have
         * partially written flash via esp_ota_write() under the CURRENT
         * handle, and esp_ota_begin() must not be called again over a handle
         * that hasn't been esp_ota_end()'d or esp_ota_abort()'d first --
         * doing so risks leaving the partition in an inconsistent state.
         * The correct way for the hub to actually switch targets is to send
         * OTA_ABORT first (handle_abort() below tears the handle down
         * cleanly via esp_ota_abort()) and only then a new OTA_BEGIN --
         * exactly what node_ota.c's finish_session() already does whenever
         * it gives up on a session before starting another. */
        ESP_LOGW(TAG, "OTA_BEGIN from " MACSTR " while a session is already active; rejecting",
                 MAC2STR(src));
        send_status(src, begin->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_ALREADY_ACTIVE, 0);
        return;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": no next update partition", MAC2STR(src));
        send_status(src, begin->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_NO_PARTITION, 0);
        return;
    }
    if (begin->total_len == 0 || begin->total_len > part->size) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": total_len %" PRIu32 " invalid for partition size %" PRIu32,
                 MAC2STR(src), begin->total_len, part->size);
        send_status(src, begin->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_TOO_LARGE, 0);
        return;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(part, begin->total_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA_BEGIN from " MACSTR ": esp_ota_begin failed: %s", MAC2STR(src), esp_err_to_name(err));
        send_status(src, begin->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_BEGIN_FAILED, 0);
        return;
    }

    memset(s, 0, sizeof(*s));
    s->active = true;
    s->handle = handle;
    s->part = part;
    s->total_len = begin->total_len;
    s->session_id = begin->session_id;
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
    ESP_LOGW(TAG, "OTA_BEGIN accepted from " MACSTR ": session=%" PRIu32 " total_len=%" PRIu32 " fw=%s -> writing %s",
             MAC2STR(src), s->session_id, s->total_len, fwv, part->label);
    send_status(src, s->session_id, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, 0);
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
        send_status(src, s->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_END_FAILED, s->next_offset);
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
        send_status(src, s->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_HASH_MISMATCH, s->next_offset);
        return;
    }

    esp_err_t berr = esp_ota_set_boot_partition(s->part);
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "OTA finalize for " MACSTR ": esp_ota_set_boot_partition failed: %s",
                 MAC2STR(src), esp_err_to_name(berr));
        s->active = false;
        send_status(src, s->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_SET_BOOT_FAILED, s->next_offset);
        return;
    }

    s->active = false;
    ESP_LOGW(TAG, "node OTA complete (from " MACSTR "), restarting to boot %s after %d DONE retransmissions",
             MAC2STR(src), s->part->label, NODE_OTA_RECV_DONE_RETRANSMITS);
    /* Retransmit the terminal DONE status (fix, M5c hardware round 4, defect
     * 1 -- see NODE_OTA_RECV_DONE_RETRANSMITS' comment above for why one
     * send is not enough) instead of sending it once. Each send is followed
     * by the same delay that used to run once, so the total time spent here
     * -- NODE_OTA_RECV_DONE_RETRANSMITS * NODE_OTA_RECV_DONE_RETRANSMIT_MS,
     * statically asserted above to fit inside NODE_OTA_RECV_RESTART_DELAY_MS
     * -- still gives the LAST send a chance to actually clear the radio
     * before esp_restart(), same reasoning as the single delay this
     * replaces (itself modeled on ota_post.c's restart_cb, just via
     * vTaskDelay() directly since this task has no in-flight HTTP response
     * to return first). */
    for (int i = 0; i < NODE_OTA_RECV_DONE_RETRANSMITS; i++) {
        send_status(src, s->session_id, OTA_ST_DONE, NODE_OTA_RECV_ERR_NONE, s->next_offset);
        vTaskDelay(pdMS_TO_TICKS(NODE_OTA_RECV_DONE_RETRANSMIT_MS));
    }
    esp_restart();
}

static void handle_chunk(recv_session_t *s, const uint8_t src[6], const swarm_ota_chunk_t *chunk)
{
    if (!s->active) {
        /* No active session (fix, M5c hardware round 4, defect 2a): this
         * used to be a silent drop, which is exactly why the hub kept
         * drain-resending chunks for ~3 minutes after round 4's node had
         * already finished and rebooted -- that traffic itself then
         * disrupted the REBOOTED node's own reading delivery (serial showed
         * repeated "reading send failed" / "resync: hub not reachable"
         * throughout). Reply with a status carrying state=OTA_ST_IDLE so the
         * hub can tell "not currently in a session" apart from silence and
         * stop on its own -- see node_ota.c's handling of OTA_ST_IDLE for
         * the other half of this fix. Rate-limited the same way as the
         * ahead-of-next_offset branch below (~once/s): a hub that keeps
         * sending after this node has nothing to write can push hundreds of
         * chunks/second (NODE_OTA_CHUNK_YIELD_MS in node_ota.c), and an
         * unthrottled reply here would just add to the exact radio
         * congestion this fix exists to relieve.
         *
         * session_id is echoed as 0: this node has no active session to
         * echo one from (a fresh boot's s_session is zero-initialised, and
         * even an idle-timed-out or aborted former session's leftover
         * session_id would be stale). node_ota.c's node_ota_handle_status()
         * accepts state=OTA_ST_IDLE regardless of session_id for exactly
         * this reason -- see its comment for why that is still safe. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - s->last_idle_status_us >= NODE_OTA_RECV_AHEAD_STATUS_MIN_US) {
            s->last_idle_status_us = now_us;
            send_status(src, 0, OTA_ST_IDLE, NODE_OTA_RECV_ERR_NONE, 0);
        }
        ESP_LOGD(TAG, "OTA_CHUNK from " MACSTR " with no active session, ignoring (rate-limited IDLE status sent)",
                 MAC2STR(src));
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
            send_status(src, s->session_id, OTA_ST_FAILED, NODE_OTA_RECV_ERR_WRITE_FAILED, s->next_offset);
            return;
        }
        mbedtls_sha256_update(&s->sha_ctx, chunk->data, chunk->len);
        s->next_offset += chunk->len;
        s->chunks_since_status++;
        s->last_activity_us = esp_timer_get_time();

        bool at_end = s->next_offset >= s->total_len;
        if (s->chunks_since_status >= NODE_OTA_RECV_STATUS_EVERY || at_end) {
            send_status(src, s->session_id, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, s->next_offset);
            s->chunks_since_status = 0;
        }
        if (at_end) finalize_session(s, src);   /* may esp_restart() and never return */
    } else if (chunk->offset < s->next_offset) {
        /* Behind next_offset: this is normal rewind-replay traffic -- the
         * hub already rewound sent_offset (in response to an earlier status
         * from THIS node, or its own stall handling) and is now re-streaming
         * a range already written. Deliberately silent: unlike the ahead
         * branch below, staying quiet here is not what causes the deadlock
         * (M5c hardware round 3) -- the hub already knows this node's real
         * offset in this case, that is WHY it rewound. Do not "fix" this
         * branch to also send a status; it would just be redundant traffic
         * on every rewind replay chunk. */
        ESP_LOGD(TAG, "OTA_CHUNK offset %" PRIu32 " behind next_offset %" PRIu32 " (rewind replay), ignoring",
                 chunk->offset, s->next_offset);
    } else {
        /* Ahead of next_offset: a chunk was lost in the radio, not a
         * decode failure -- M5c hardware round 3 ran with a frame-level rx
         * diagnostic active and it showed dropped=0 (every arriving frame
         * decoded fine, e.g. "rx: wire_type=8 len=208 -> type=8"); the
         * chunks that never showed up simply never arrived over the air.
         * Once that happens, EVERY later chunk in the transfer lands in
         * this branch forever (next_offset can't advance without the
         * missing one), so chunks_since_status in the accepted branch above
         * never reaches NODE_OTA_RECV_STATUS_EVERY again -- this node goes
         * completely silent. The hub has no way to learn where to rewind
         * to, keeps streaming into the void, burns its stall budget waiting
         * for a status that will never come on its own, and aborts. That is
         * exactly the observed hardware plateau: hub sent 1158800/1158800
         * (100%), node had written only 985600 (85%), hub aborted err=5
         * (stall).
         *
         * Fix: proactively report the CURRENT next_offset from right here
         * so the hub can rewind -- rate-limited to roughly once a second.
         * A lost chunk means every subsequent chunk (there can be hundreds,
         * given node_ota.c's 2ms inter-chunk pacing, before the hub's own
         * pacing/backoff even reacts) lands in this branch, so an
         * unthrottled status per chunk here would flood the radio with
         * exactly the kind of traffic PlanV1 8e already warns about --
         * likely making delivery worse, not better. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - s->last_ahead_status_us >= NODE_OTA_RECV_AHEAD_STATUS_MIN_US) {
            s->last_ahead_status_us = now_us;
            send_status(src, s->session_id, OTA_ST_RECEIVING, NODE_OTA_RECV_ERR_NONE, s->next_offset);
        }
        ESP_LOGD(TAG, "OTA_CHUNK offset %" PRIu32 " ahead of next_offset %" PRIu32
                      ", ignoring (rate-limited status will pull the hub back)",
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
