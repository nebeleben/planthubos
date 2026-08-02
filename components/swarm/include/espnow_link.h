#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Thin transport layer over ESP-NOW: peer management, channel control and a
 * blocking send. Callers above this layer (pairing, reading upload) own all
 * protocol semantics; this component only moves bytes and manages peers.
 *
 * Threading: espnow_link_init() registers the ESP-NOW recv/send callbacks,
 * which run on the WiFi driver task. The receive callback registered here
 * only decodes RSSI and hands the frame to the caller-supplied espnow_rx_cb_t
 * — it must return quickly. In particular espnow_link_send()/broadcast()
 * must NEVER be called from inside that callback: they block on a
 * semaphore signalled by the ESP-NOW send callback, which itself runs on
 * the same WiFi task, so calling send from the recv callback would
 * deadlock the WiFi task against itself. Callers that need to reply to a
 * received frame must defer the reply to their own task (see pairing.c).
 */

#define ESPNOW_BROADCAST_MAC { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

typedef void (*espnow_rx_cb_t)(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi);

/* Assumes WiFi is already initialised and started (STA or AP+STA). Sets the
 * ESP-NOW PMK, adds the (unencrypted) broadcast peer, and re-adds every peer
 * already known to swarm_store (the hub if this device is a node, or the
 * node table if this device is the hub). Safe to call once at boot. */
esp_err_t espnow_link_init(espnow_rx_cb_t cb);

/* lmk == NULL means an unencrypted peer (used for the broadcast address;
 * ESP-NOW cannot encrypt broadcast traffic). channel == 0 means "use
 * whatever channel the radio is currently on" — preferred over pinning a
 * peer to a fixed channel so it keeps working across a node's channel
 * sweeps/resyncs. Adds the peer, or updates it in place if it already
 * exists. */
esp_err_t espnow_link_add_peer(const uint8_t mac[6], const uint8_t *lmk, uint8_t channel);

/* Blocks until the ESP-NOW send callback reports the outcome of this frame
 * or 200 ms elapse, whichever comes first. Returns ESP_OK only when the
 * callback reported ESP_NOW_SEND_SUCCESS. Must not be called from the
 * espnow_rx_cb_t callback (see header note above). */
esp_err_t espnow_link_send(const uint8_t mac[6], const uint8_t *data, size_t len);

/* Sends to the broadcast peer. Same blocking/threading rules as
 * espnow_link_send(); note ESP-NOW broadcast has no MAC-layer ACK, so
 * ESP_OK here only means the frame was queued and handed to the radio, not
 * that anyone received it. */
esp_err_t espnow_link_broadcast(const uint8_t *data, size_t len);

/* Node-only: changes the WiFi/ESP-NOW channel. Refuses (ESP_ERR_INVALID_STATE)
 * when this device is currently associated to an AP as a station, because
 * the hub's channel follows its STA association and changing it here would
 * break that link. A node that has not associated to any AP (the normal
 * case for an M5a node) is free to hop channels while pairing/resyncing. */
esp_err_t espnow_link_set_channel(uint8_t ch);

/* Current primary WiFi channel, via esp_wifi_get_channel(). */
uint8_t espnow_link_channel(void);
