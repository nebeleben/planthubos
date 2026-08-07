#pragma once
#include "esp_err.h"
#include "swarm_frame.h"
#include <stdint.h>

/* Node-side OTA receiver (M5c Task 5): the other half of node_ota.h's
 * hub-side push session. Accepts OTA_BEGIN/OTA_CHUNK/OTA_ABORT from this
 * node's stored hub over ESP-NOW, writes the image to the next update
 * partition, verifies it, and reboots into it -- or aborts and stays on the
 * current image. See the plan (Task 5) for the full behavioural spec and
 * node_ota.h for the hub-side session this mirrors.
 *
 * A DELIBERATE SIBLING FILE to node_ota.c, not an addition to it:
 * node_ota.c's own header comment documents it as "Hub-side node OTA
 * session" and its state (ota_session_t, s_session) is hub-only. Keeping
 * the two directions in separate files matches that existing split instead
 * of overloading one file/one set of statics with both roles' state
 * machines, which would be easy to get backwards under time pressure (this
 * is, after all, the one place a mistake silently reverts an update -- see
 * the rollback-guard wiring in ota_post.h/swarm.c instead, which is the
 * actually critical piece here, not this file's naming).
 *
 * Threading: node_ota_recv_handle_begin()/handle_chunk()/handle_abort() are
 * called directly from swarm.c's node_rx_cb() -- the ESP-NOW receive
 * callback, i.e. the WiFi driver task -- and therefore must NEVER write
 * NVS, block on flash, or send (project-wide rule). Each of them only
 * performs a cheap, bounded, allocation-free RAM check (sender MAC against
 * swarm_store_hub(), no NVS touched -- same reasoning already applied to
 * every other swarm_store read from this exact callback path elsewhere in
 * the codebase) and then a non-blocking enqueue onto a queue that
 * node_ota_recv_task() -- a dedicated FreeRTOS task started by
 * node_ota_recv_init() -- drains. Every esp_ota_begin/write/end,
 * esp_ota_set_boot_partition, and espnow_link_send(OTA_STATUS) happens on
 * that task, never on the callback. A full queue simply drops the event:
 * self-healing, since the periodic OTA_STATUS this node sends back (or the
 * hub's own 5s stall timer) will pull the hub's go-back-N sender back to
 * whatever offset this node actually landed on. */

/* Starts the dedicated receiver task + its queue. Idempotent; call once,
 * BEFORE espnow_link_init() brings up the receive callback that could hand
 * this component its first frame (see swarm_start_node()). */
esp_err_t node_ota_recv_init(void);

/* Called only from swarm.c's node_rx_cb() (the ESP-NOW receive callback).
 * Rejects (silently) anything not from this node's stored hub MAC -- the
 * plan's explicit requirement -- then hands off to the receiver task. */
void node_ota_recv_handle_begin(const uint8_t src[6], const swarm_ota_begin_t *begin);
void node_ota_recv_handle_chunk(const uint8_t src[6], const swarm_ota_chunk_t *chunk);
void node_ota_recv_handle_abort(const uint8_t src[6], const swarm_ota_abort_t *ab);

/* M7 Task 5: whether a receive session is currently active. Consumed by
 * swarm.c's swarm_node_battery_cycle() (a THIRD task, distinct from both
 * node_rx_cb's WiFi driver task and node_ota_recv_task() above) to poll,
 * once a second, whether a STAY_AWAKE wake should keep waiting for an OTA
 * push to finish. recv_session_t itself stays exactly as task-owned-only as
 * this file's header comment already documents -- this getter reads a
 * separate, single bool written only from node_ota_recv_task() at the same
 * points s_session.active itself changes, never the struct. No lock: a
 * plain bool read/write is indivisible on this target, and the only
 * consumer polls at 1s granularity expecting eventual, not immediate,
 * consistency (see swarm.c's stay-awake wait loop) -- a stale read is
 * corrected on the very next poll, one second later, which is harmless at
 * this timescale. Safe to call from any task. */
bool node_ota_recv_active(void);

/* Node's own OTA_STATUS.err codes (the numberspace node_ota.h's
 * node_ota_progress_t.err comment defers to "the node side's to define,
 * Task 5"). Sent verbatim in a FAILED OTA_STATUS; the hub surfaces it
 * as-is in node_ota_progress_t.err. */
enum {
    NODE_OTA_RECV_ERR_NONE            = 0,
    NODE_OTA_RECV_ERR_ALREADY_ACTIVE  = 1,  /* OTA_BEGIN while a session is already running */
    NODE_OTA_RECV_ERR_NO_PARTITION    = 2,  /* esp_ota_get_next_update_partition() failed */
    NODE_OTA_RECV_ERR_TOO_LARGE       = 3,  /* total_len exceeds the partition */
    NODE_OTA_RECV_ERR_BEGIN_FAILED    = 4,  /* esp_ota_begin() failed */
    NODE_OTA_RECV_ERR_WRITE_FAILED    = 5,  /* esp_ota_write() failed */
    NODE_OTA_RECV_ERR_END_FAILED      = 6,  /* esp_ota_end() (image validation) failed */
    NODE_OTA_RECV_ERR_HASH_MISMATCH   = 7,  /* streamed sha256 != OTA_BEGIN's */
    NODE_OTA_RECV_ERR_SET_BOOT_FAILED = 8,  /* esp_ota_set_boot_partition() failed */
    NODE_OTA_RECV_ERR_ABORTED_BY_HUB  = 9,  /* OTA_ABORT received mid-session */
};
