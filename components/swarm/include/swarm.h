#pragma once
#include "esp_err.h"
#include "radio_role_str.h"
#include <stdbool.h>
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

/* Hub (M7): the given node's last self-reported power mode (SWARM_PM_*,
 * swarm_store.h), from its most recent accepted CHECKIN this boot -- the
 * same RAM stats slot GET /api/v1/nodes' "reported_mode"/"reported_mode_valid"
 * fields come from (swarm_node_list_json()). Returns false (mode_out
 * untouched) if this node has never checked in this boot -- callers must
 * treat that as unknown, not as "reported ALWAYS_ON". node_ota.c's
 * node_ota_start() uses this to decide whether a push should park
 * (NODE_OTA_ST_PENDING_WAKE) rather than stream immediately. Safe to call
 * from any task. */
bool swarm_node_reported_mode(const uint8_t mac[6], uint8_t *mode_out);

/* Hub (M7 Task 4): the given node's last self-reported radio role
 * (RADIO_ROLE_*, radio_role_str.h), learned from its most recent PAIR_REQ
 * (pairing.c, via swarm_note_node_radio() below) or COORD_STATUS (Task 7)
 * this boot -- the same RAM stats slot GET /api/v1/nodes'
 * "reported_radio_role"/"radio_role_pending" fields come from
 * (swarm_node_list_json()). Returns false (role_out untouched) if this
 * node has never reported a radio role this boot -- callers (api_v1.c's
 * node_update_post(), reconciling a power_mode change against
 * swarm_rules_node_power_ok()) must fall back to
 * swarm_store_node_desired_radio() in that case, same "unknown, not a
 * guess" reasoning as swarm_node_reported_mode() above. Safe to call from
 * any task. */
bool swarm_node_reported_radio(const uint8_t mac[6], uint8_t *role_out);

/* Hub (M7 Task 4): records a node's self-reported radio role into its RAM
 * stats slot -- a no-op if that node has no slot yet (e.g. a PAIR_REQ from
 * a node that has never sent a READING/CHECKIN this boot; see swarm.c's
 * record_reported_radio()). Called from pairing.c right after a hub-side
 * PAIR_REQ is decoded (the frame's own radio_role field), and from Task 7's
 * COORD_STATUS handling. Safe to call from the ESP-NOW receive callback:
 * same short, bounded, allocation-free s_stats_mutex critical section as
 * record_stat()/record_checkin_mode(). */
void swarm_note_node_radio(const uint8_t mac[6], uint8_t r);

/* Hub (M7 Task 4): encodes and sends a NODE_CONFIG directing `mac` to run
 * radio role `r`, with a fresh per-node sequence number (node_stat_t.cfg_seq)
 * so the NODE_CONFIG_ACK the node replies with (hub_rx_cb) can be matched
 * to it in the log. Must be called from a task context (checkin_task, or
 * the HTTP handler's own task via swarm_request_node_config() below) --
 * never from the ESP-NOW receive callback, since espnow_link_send() can
 * block. Returns ESP_ERR_NO_MEM if this node has no RAM stats slot and none
 * is free (SWARM_MAX_NODES exceeded, the same corner case record_stat()
 * itself tolerates), or whatever espnow_link_send() returns on a send
 * failure -- either way this is best-effort: a dropped NODE_CONFIG is
 * retried the next time checkin_task notices desired != reported. */
esp_err_t swarm_send_node_config(const uint8_t mac[6], radio_role_t r);

/* Hub (M7 Task 4): queues an immediate NODE_CONFIG for `mac`, drained by
 * checkin_task (a config_only checkin_item_t, sent with no CHECKIN_ACK --
 * see checkin_task()'s own comment) rather than waiting for that node's
 * next CHECKIN. Called from api_v1.c's node_update_post() right after a
 * POST /api/v1/nodes/{MAC12} {"radio_role":...} persists a new desired
 * role, so an awake node gets it without a full checkin round-trip.
 * ESP_ERR_INVALID_STATE if the checkin queue doesn't exist yet (hub not
 * started as main), ESP_ERR_NO_MEM if it's momentarily full -- both
 * best-effort, same reasoning as swarm_send_node_config() above: a missed
 * request is just picked up by this node's next ordinary checkin. */
esp_err_t swarm_request_node_config(const uint8_t mac[6]);

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

/* Node-only (M5c node-side OTA rollback guard -- see ota_post.h). Registers
 * a callback invoked the first time this node proves itself "healthy"
 * after boot: forward_task() successfully delivering a reading to the hub,
 * or receiving a PONG from it. Call BEFORE swarm_start_node() so nothing
 * can fire before a callback is armed; a NULL cb clears it (also this
 * component's own initial state, so wiring this up is opt-in and a hub
 * never has one set -- swarm_start_main() never calls the callback). The
 * callback is always invoked from a dedicated FreeRTOS task, never from
 * the ESP-NOW receive callback (see swarm.c's health-confirm task), so it
 * is safe for the callback to do flash I/O (as ota_post.h's
 * ota_rollback_guard_node_confirm() does). Deliberately decoupled via a
 * function pointer rather than swarm.c calling into webserver/ota_post.h
 * directly: the webserver component already depends on swarm (api_v1.c),
 * so the reverse dependency would be circular -- main.c, which depends on
 * both, does the wiring. */
void swarm_node_set_health_cb(void (*cb)(const char *reason));

/* Battery-mode wake cycle (spec §4). Called from main.c's node-paired
 * branch INSTEAD OF returning to a plain always-on run, when
 * swarm_store_power_mode() != SWARM_PM_ALWAYS_ON. Runs the bounded wake
 * (scan/forward via the already-started node machinery), sends CHECKIN,
 * applies the ack, and either deep-sleeps (never returns) or returns
 * ESP_OK meaning "continue as always-on" (mode changed / fallback /
 * stay-awake ended). Requires swarm_start_node() to have succeeded.
 *
 * Threading: main.c runs this from its own dedicated "batt_cycle" task
 * (app_main() itself must still return), never directly from app_main --
 * see main.c's own comment on that task for why. This function performs
 * NVS writes (wake counter, failed wakes, power mode) and blocking sends,
 * neither of which is safe on node_rx_cb (the ESP-NOW receive callback,
 * WiFi driver task) -- that callback's only involvement is queuing a
 * decoded CHECKIN_ACK for this function to pick up. */
esp_err_t swarm_node_battery_cycle(void);
