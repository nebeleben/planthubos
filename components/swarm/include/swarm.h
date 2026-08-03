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

/* Hub: called when a node is forgotten (api_v1.c's DELETE handler) -- clears
 * that MAC's per-node RAM stats slot (frames_rx/last_seen_s/rssi), if it has
 * one, under the same mutex record_stat() uses. Without this, a forgotten
 * node's slot stays "in_use" for the rest of this boot: once all
 * SWARM_MAX_NODES slots have ever been occupied, a replacement node paired
 * later can never get one of its own, and GET /api/v1/nodes would show it
 * stuck at frames_rx=0/last_seen_s=null forever even as
 * frames_rx_total keeps climbing. A no-op if the node never transmitted (no
 * slot to clear). Called only from the forget HTTP handler's task -- never
 * from the ESP-NOW receive callback, which only ever calls record_stat(). */
void swarm_forget_node_stats(const uint8_t mac[6]);

/* Hub: broadcasts SWARM_MSG_FORGET (carrying `mac` as its target_mac -- see
 * swarm_frame.h) a few times (from a dedicated task, not the caller) so a
 * still-powered node learns it was forgotten and returns to its portal on
 * its own, instead of believing it is paired forever (M5b's gap -- see
 * PlanV1 8f). Call AFTER swarm_store_forget_node() and
 * espnow_link_remove_peer() have already run for `mac` -- this function
 * does not touch either itself. Best-effort/fire-and-forget: a node that
 * was powered off at the time still needs the physical BOOT-button
 * recovery; this does not replace that path, only shortcuts it when the
 * node happens to be listening. Every node paired to this hub still
 * receives the broadcast (ESP-NOW has no way to unicast to a peer that was
 * just removed), but target_mac now scopes WHICH one acts on it -- only the
 * node whose own STA MAC equals `mac` unpairs; see swarm.c's
 * forget_broadcast_task() and pairing.c's FORGET handling. */
void swarm_broadcast_forget(const uint8_t mac[6]);
