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

/* Opens a pairing window for `seconds`. The first valid PAIR_REQ seen while
 * open starts an adoption attempt (LMK generated, node stored, ESP-NOW peer
 * added, PAIR_ACK sent back); at most one attempt runs at a time, and the
 * window closes only once an attempt fully succeeds, so a failure partway
 * through (NVS, peer table, send) doesn't silently burn the rest of the
 * window -- the same or another node can still retry before it expires.
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

/* Starts a task that sweeps channels 1..13 (~300 ms dwell each), broadcasting
 * a fresh PAIR_REQ at the start of every full sweep, until a PAIR_ACK with a
 * matching nonce arrives (-> PAIR_OK, hub stored, ESP-NOW peer added) or
 * `timeout_s` elapses (-> PAIR_FAILED). Returns an error only if the task
 * could not be created (e.g. one is already running); the outcome of the
 * search itself is read via pairing_node_state(). */
esp_err_t pairing_node_start(uint32_t timeout_s);

pairing_state_t pairing_node_state(void);

/* Node-side channel recovery: sweeps channels 1..13 sending SWARM_MSG_PING
 * to the stored hub MAC (blocking on espnow_link_send per channel) and
 * persists the first channel a send succeeds on via swarm_store_set_channel().
 * Blocking; call from a task, never from the espnow_rx_cb_t receive
 * callback. Returns ESP_ERR_NOT_FOUND if no channel got a delivered send,
 * ESP_ERR_INVALID_STATE if this node has no stored hub. Intended to be
 * called by the reading-upload path after N consecutive send failures. */
esp_err_t pairing_node_resync_channel(void);
