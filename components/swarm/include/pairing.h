#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Hub-window / node-sweep pairing handshake on top of espnow_link.
 *
 * Threading: pairing_handle_frame() is meant to be called directly (or via
 * a thin dispatcher) from the espnow_rx_cb_t passed to espnow_link_init(),
 * i.e. it runs on the WiFi driver task. It only ever inspects the frame,
 * takes a short-lived lock to stash a couple of fields, and signals a
 * FreeRTOS primitive — it never calls swarm_store_add_node()/set_hub()
 * (flash writes) or espnow_link_send()/broadcast() (which block waiting on
 * a signal from that same WiFi task). All of that work — including
 * generating the LMK and sending PAIR_ACK on the hub, and persisting the
 * hub record and adding the ESP-NOW peer on the node — happens on
 * dedicated pairing tasks owned by this component, which pairing_handle_frame
 * only wakes up via a queue/semaphore. See pairing.c for the task bodies. */

typedef enum {
    PAIR_IDLE,
    PAIR_SEARCHING,
    PAIR_OK,
    PAIR_FAILED,
} pairing_state_t;

/* --- Hub side --- */

/* Brings up the SWARM_MSG_PING -> SWARM_MSG_PONG responder used for
 * node-side liveness/resync (pairing_node_resync_channel()). Unlike
 * pairing_open_window(), this is NOT gated behind an operator opening a
 * pairing window -- a node may probe for the hub at any time, so
 * swarm_start_main() calls this once, unconditionally, right after
 * espnow_link_init(). Idempotent; safe to call more than once. Returns an
 * error only if the responder task/queue could not be created. */
esp_err_t pairing_hub_init(void);

/* Opens a pairing window for the full `seconds` duration -- it is NEVER
 * closed early, including on a successful adoption. Every valid PAIR_REQ
 * seen while open starts an adoption attempt (ESP-NOW peer added, PAIR_ACK
 * sent, then persisted -- see hub_task() in pairing.c for the exact order
 * and why); at most one attempt runs at a time (the s_adopt_in_progress
 * guard). Adoption is idempotent per MAC: a PAIR_REQ from a MAC already in
 * swarm_store's node table reuses the STORED LMK and just re-sends
 * PAIR_ACK rather than generating a new key, which matters because a node
 * whose first PAIR_ACK never arrived (radio noise, a slow persist, bad
 * luck) keeps sweeping and re-requesting with the SAME already-adopted
 * MAC -- generating a fresh key on that retry would desync it from the
 * key the hub already committed to flash. Leaving the window open for its
 * full duration (rather than closing on the first success) is what lets
 * that retry actually reach the hub instead of being silently ignored.
 * Starts this component's hub pairing task on first call. */
void pairing_open_window(uint32_t seconds);

bool pairing_window_open(void);
uint32_t pairing_window_remaining_s(void);

/* Feed every received ESP-NOW frame here (from the shared espnow_rx_cb_t).
 * Handles PAIR_REQ (only meaningful while pairing_window_open()) and
 * PAIR_ACK (only meaningful while pairing_node_state() == PAIR_SEARCHING);
 * anything else, or a frame that doesn't decode, is silently ignored.
 * Safe to call unconditionally from the receive callback on both hub and
 * node builds. Must not block (see threading note above). */
void pairing_handle_frame(const uint8_t src[6], const uint8_t *data, int len, int rssi);

/* --- Node side --- */

/* Starts a task that sweeps channels 1..13 (~500 ms dwell each -- widened
 * from an original 300 ms on real-hardware evidence that a hub's PAIR_ACK,
 * which waits behind a synchronous NVS commit, could arrive after a 300 ms
 * dwell had already moved the node to the next channel; the hub now sends
 * before persisting, but the wider dwell is kept as a second, independent
 * margin against a slow/busy channel), broadcasting a fresh PAIR_REQ at the
 * start of every full sweep, until a PAIR_ACK with a matching nonce arrives
 * (-> PAIR_OK, hub stored, ESP-NOW peer added) or `timeout_s` elapses
 * (-> PAIR_FAILED). The sweep is bounded purely by wall-clock `timeout_s`
 * (not a fixed sweep/channel count), so a wider dwell simply means fewer
 * full sweeps fit in the same timeout, with no separate count to keep in
 * sync. Returns an error only if the task could not be created (e.g. one
 * is already running); the outcome of the search itself is read via
 * pairing_node_state(). */
esp_err_t pairing_node_start(uint32_t timeout_s);

pairing_state_t pairing_node_state(void);

/* Node-side channel recovery: sweeps channels 1..13, on each sending a
 * SWARM_MSG_PING (blocking on espnow_link_send) to the stored hub MAC and
 * then waiting briefly for a matching SWARM_MSG_PONG (nonce-matched, same
 * as PAIR_ACK). Persists the first channel that gets an actual PONG via
 * swarm_store_set_channel() -- NOT merely the first channel whose PING was
 * MAC-acked. That distinction is the whole point: M5a's version trusted a
 * delivered PING (send() returning ESP_OK) as proof the hub was reachable,
 * but a MAC-layer ack only proves the hub's RADIO received the frame --
 * ESP-NOW itself can still silently discard it (unknown peer, undecryptable
 * LMK mismatch) before the hub's application layer ever sees it. Only a
 * PONG proves the round trip actually happened. Blocking; call from a
 * task, never from the espnow_rx_cb_t receive callback. Returns
 * ESP_ERR_NOT_FOUND if no channel got a PONG, ESP_ERR_INVALID_STATE if
 * this node has no stored hub. Intended to be called by the
 * reading-upload path after N consecutive send failures. */
esp_err_t pairing_node_resync_channel(void);
