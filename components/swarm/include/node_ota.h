#pragma once
#include "esp_err.h"
#include "swarm_frame.h"
#include <stdbool.h>
#include <stdint.h>

/* Hub-side node OTA session (M5c): pushes the hub's OWN running firmware
 * image to a paired node over ESP-NOW, go-back-N, so the node always ends
 * up on exactly what the hub itself runs -- no second artifact to build,
 * store, or version-skew against. See the plan
 * (docs/superpowers/plans/2026-08-03-planthub-m5c-node-ota.md, Task 4) for
 * the full behavioural spec this implements.
 *
 * Threading: node_ota_start() only validates preconditions (RAM-only,
 * fast) and spawns a dedicated task that owns the entire transfer --
 * reading the running partition, hashing, and every espnow_link_send()
 * call happen there, never on the caller's task. node_ota_handle_status()
 * is called from the ESP-NOW receive callback (the WiFi driver task) and
 * therefore must NEVER send, write NVS, or block on flash -- it only
 * records the latest status under a non-blocking mutex attempt. All public
 * functions here are safe to call from any task except the receive
 * callback itself (which must only ever call node_ota_handle_status()). */

typedef struct {
    bool     active;        /* a session is currently running (task alive, streaming) */
    uint8_t  mac[6];        /* target node -- valid whenever a session has ever run this boot */
    uint32_t total_len;     /* true image length, once known (0 before OTA_BEGIN is sent) */
    uint32_t sent_offset;   /* hub's go-back-N send cursor */
    uint32_t acked_offset;  /* highest next_offset the node has actually confirmed */
    uint8_t  state;         /* one of OTA_ST_* (swarm_frame.h) -- the hub's own view:
                              * RECEIVING while a session is in flight, DONE/FAILED once
                              * it ends. IDLE before any session has ever run. */
    uint8_t  err;           /* meaningful only when state == OTA_ST_FAILED. Either a
                              * NODE_OTA_ERR_* code (see below) for a hub-detected
                              * failure, or the node's own OTA_STATUS.err byte verbatim
                              * when the node itself reported OTA_ST_FAILED (that
                              * numberspace is the node side's to define, Task 5) --
                              * the two are not disambiguated by this struct alone. */
    uint32_t started_s;     /* esp_timer_get_time()/1e6 when this session started */
} node_ota_progress_t;

/* Hub-detected failure codes for node_ota_progress_t.err. Distinct from
 * swarm_ota_status_t's own state/err (the node's protocol-level error,
 * defined on the node side, Task 5), which this hub session also surfaces
 * verbatim in the same field when the failure was node-reported rather
 * than hub-detected -- see the struct comment above. */
enum {
    NODE_OTA_ERR_NONE          = 0,
    NODE_OTA_ERR_NO_PARTITION  = 1,  /* esp_ota_get_running_partition() failed */
    NODE_OTA_ERR_IMAGE_LEN     = 2,  /* couldn't determine the true image length (esp_image_get_metadata) */
    NODE_OTA_ERR_READ          = 3,  /* esp_partition_read() failed mid-session */
    NODE_OTA_ERR_BEGIN_SEND    = 4,  /* OTA_BEGIN never got out after repeated retries */
    NODE_OTA_ERR_STALL         = 5,  /* NODE_OTA_MAX_STALLS consecutive OTA_STATUS reports whose
                                       * next_offset failed to advance since the previous one (M5c
                                       * hardware round 3 fix -- see node_ota.c's stall-handling
                                       * comment; was time-based silence before that) */
    NODE_OTA_ERR_TIMEOUT       = 6,  /* 10 minute total session timeout */
    NODE_OTA_ERR_ABORTED       = 7,  /* node_ota_abort() was called */
    NODE_OTA_ERR_NO_MEM        = 8,  /* task/session setup failed */
    NODE_OTA_ERR_SESSION_LOST  = 9,  /* M5c hardware round 4, defect 2: the node is no longer
                                       * receiving this session. Reached two ways -- (a) an
                                       * OTA_STATUS with state=OTA_ST_IDLE arrived for the current
                                       * session (node_ota_handle_status() below accepts this
                                       * regardless of session_id -- a node with no active session
                                       * cannot echo one back, most commonly because it already
                                       * finished, rebooted, and lost all RAM session state before
                                       * its own terminal DONE status got through; see
                                       * node_ota_recv.c's handle_chunk() !active branch and
                                       * finalize_session()'s DONE retransmission), or (b)
                                       * node_ota_task()'s drain phase ran too many consecutive
                                       * passes with NO status at all from the node. Distinct from
                                       * NODE_OTA_ERR_STALL: STALL means the node IS reporting, just
                                       * without progress; SESSION_LOST means the hub has no
                                       * evidence at all that the node is still there. NOTE: if
                                       * every DONE retransmission the node sends is lost, the hub
                                       * reaches this same code for an update that actually
                                       * succeeded -- that is the correct, honest outcome given the
                                       * hub has no other completion evidence, and (unlike before
                                       * this fix) it is now reached in seconds, not minutes. */
};

/* Begins a session pushing the hub's running firmware to node_mac. Returns
 * quickly (RAM-only preconditions, no flash/network I/O on the caller's
 * task) after spawning a dedicated task that owns the actual transfer.
 * ESP_ERR_INVALID_STATE if a session is already active. ESP_ERR_NOT_FOUND
 * if node_mac is not in swarm_store's paired-node table. ESP_ERR_NO_MEM on
 * allocation/task-creation failure. */
esp_err_t node_ota_start(const uint8_t node_mac[6]);

/* Snapshots the current (or, once finished, the last) session's state into
 * *out. Safe to call from any task; never blocks on flash/network. */
void node_ota_progress(node_ota_progress_t *out);

/* Called ONLY from the ESP-NOW receive callback (the WiFi driver task).
 * Records the status if a session targeting `src` is currently active;
 * otherwise a silent no-op. MUST NEVER send, write NVS, or block on flash --
 * uses a non-blocking mutex attempt internally specifically so it can never
 * stall the caller even transiently. */
void node_ota_handle_status(const uint8_t src[6], const swarm_ota_status_t *st);

/* Requests the active session (if any) stop as soon as its task next checks
 * -- best-effort OTA_ABORT is sent to the node, state becomes
 * OTA_ST_FAILED/NODE_OTA_ERR_ABORTED, and the task exits. Returns
 * ESP_ERR_INVALID_STATE if no session is active. Does not block waiting
 * for the task to actually finish tearing down. */
esp_err_t node_ota_abort(void);

/* Hub-side-only pseudo-state, never seen on the wire (M7). node_ota_start()
 * parks a session in this state -- rather than sending OTA_BEGIN and
 * streaming immediately -- when the target is a battery node that's
 * presumed asleep between checkins (see node_ota.c's node_ota_start() for
 * the exact condition: swarm_store_node_desired_mode() != ALWAYS_ON, or its
 * last-reported mode, per swarm.c's swarm_node_reported_mode(), is a
 * battery mode). The session's task blocks on an internal semaphore until
 * swarm.c's checkin_task() calls node_ota_notify_checkin() for this node's
 * next CHECKIN, at which point it proceeds exactly as an unparked session
 * would. node_ota_progress() reports this state like any other OTA_ST_*
 * value; a caller not expecting it (e.g. code written before M7) sees an
 * unrecognised state number, not a crash. */
#define NODE_OTA_ST_PENDING_WAKE 4

/* Called by swarm.c's checkin_task() on EVERY accepted CHECKIN (not only
 * ones that resolve to STAY_AWAKE) -- see checkin_task() for why this is
 * fine to call unconditionally: it's cheap and a no-op unless a session for
 * `mac` is actually parked. Returns true if a parked session (state ==
 * NODE_OTA_ST_PENDING_WAKE) targeting `mac` existed and was just released
 * -- the caller (checkin_task()) uses this only to decide logging/nothing
 * further; the session itself resumes on its own task regardless of
 * whether the caller does anything with the return value. Thread-safe,
 * RAM-only, and never blocks meaningfully (a short mutex critical section,
 * same discipline as every other accessor in this file). */
bool node_ota_notify_checkin(const uint8_t mac[6]);

/* Query used by swarm.c's checkin_task() to decide whether to answer a
 * CHECKIN with STAY_AWAKE: true iff a session targeting `mac` is currently
 * parked (state == NODE_OTA_ST_PENDING_WAKE). Idempotent, RAM-only, safe to
 * call from any task. */
bool node_ota_pending_for(const uint8_t mac[6]);

/* M7 final-review fix (F1/F2): companion to node_ota_pending_for() above --
 * true iff a session targeting `mac` is currently ACTIVELY STREAMING
 * (pub.active, any state other than the parked pseudo-state), as opposed to
 * pending's "parked, not yet streaming". swarm.c's checkin_task() ORs the
 * two together as batt_reconcile()'s ota_pending input so STAY_AWAKE keeps
 * winning over SET_MODE for the whole lifetime of a session -- not only
 * while parked -- covering two failure modes a parked-only check misses:
 * (a) an always-on node's periodic checkin landing a SET_MODE ack (and
 * esp_restart()) while this hub is mid-stream to it would abort the
 * transfer; (b) a battery node that misses the STAY_AWAKE ack that released
 * its park would otherwise see node_ota_pending_for() already false (the
 * session moved past PENDING_WAKE the instant it was released) and could
 * deep-sleep on its very next checkin while the hub is still streaming to
 * it. Idempotent, RAM-only, s_mutex-guarded, safe to call from any task --
 * same discipline as node_ota_pending_for(). */
bool node_ota_active_for(const uint8_t mac[6]);
