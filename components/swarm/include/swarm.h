#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Ties espnow_link, pairing and swarm_store to data_core/registry so a hub
 * ingests node-forwarded readings and a node forwards locally-heard ones,
 * per the role this device is configured with (see swarm_store).
 *
 * Threading: both start functions run once, from app_main() (or an HTTP
 * handler's task, for role changes), and are not expected to be called
 * concurrently with each other. Once started, all further work happens on
 * dedicated tasks/callbacks owned by this component and the layers below
 * it -- callers never need to poll or drive anything after start. */

/* Hub (role UNSET or MAIN): brings up ESP-NOW on top of already-started
 * WiFi, dispatching PAIR_* frames to pairing_handle_frame() and
 * SWARM_MSG_READING frames into data_core via the same door the local BLE
 * collector uses. Call after wifi_manager_start(). */
esp_err_t swarm_start_main(void);

/* Node (role NODE, already paired -- see swarm_store_hub()): brings up
 * radio-only WiFi (no STA/AP association, so no web server/sampler/
 * wifi_manager involvement), restores the last-known channel, starts
 * ESP-NOW and subscribes to PLANTHUB_DATA_EVENT to forward locally-heard
 * readings to the stored hub. Returns ESP_ERR_INVALID_STATE if this device
 * has no stored hub -- callers must check swarm_store_hub() first and run
 * the normal portal instead in that case. */
esp_err_t swarm_start_node(void);

/* Node (role NODE, NOT yet paired, and swarm_store_pair_failed() is false):
 * brings up the same radio-only WiFi + ESP-NOW as swarm_start_node(), then
 * actively searches for a hub via pairing_node_start(). A watcher task
 * (started internally) reboots this device once the search resolves --
 * clearing the pair-failed flag and restarting on success (comes back up
 * as a paired node via swarm_start_node()), or setting the flag and
 * restarting on failure/timeout (comes back up in the normal portal so a
 * human can see the failure and retry via POST /api/v1/pair/retry).
 * Callers must check swarm_store_pair_failed() themselves first and run
 * the portal directly instead when it's already set -- this function does
 * not check it, so it never owns that decision. */
esp_err_t swarm_start_node_search(void);

/* Hub: writes the GET /api/v1/nodes response body into buf (NUL-terminated
 * on success). Returns the number of bytes written (excluding the NUL), or
 * -1 if buf was too small or JSON construction failed. */
int swarm_node_list_json(char *buf, size_t cap);

/* Hub: total SWARM_MSG_READING frames ingested from nodes since boot
 * (verification aid; also the "frames_rx_total" field of the JSON above). */
uint32_t swarm_frames_rx(void);
