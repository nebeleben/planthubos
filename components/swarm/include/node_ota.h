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
