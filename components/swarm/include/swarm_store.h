#pragma once
#include "esp_err.h"
#include "swarm_frame.h"
#include <stdbool.h>
#include <stdint.h>

/* Hardware ceiling, not a design choice: ESP-NOW hard-caps ENCRYPTED peers at
 * ESP_NOW_MAX_ENCRYPT_PEER_NUM (6, per esp_now.h) regardless of chip/IDF
 * config. Every adopted node's peer gets upgraded to encrypted right after
 * PAIR_ACK (see pairing.c's hub_task()), so a 7th or 8th adoption would
 * always fail at that upgrade step -- raising this past 6 would advertise a
 * cap the radio can never actually honour. See espnow_link.c for the
 * compile-time assert tying this to that constant so the two can never
 * silently drift apart again. (An earlier draft of this task set this to 8;
 * corrected before any device was ever flashed with that shape -- the only
 * migration path that matters in practice is the M5a 89-byte one, which
 * this change does not affect.) */
#define SWARM_MAX_NODES 6

/* On-disk format of the persisted node table (KEY_NODES blob). Bump this
 * and add a migration branch in swarm_store.c's load whenever the layout
 * changes again -- M5a accepted a stored blob on exact-length match only,
 * which is why simply raising SWARM_MAX_NODES here would otherwise have
 * silently wiped every already-paired node's table on upgrade (a longer
 * fixed-size array changes the blob's length). See swarm_store.c for the
 * migration that reads an M5a-format blob (no format byte, exact old
 * length) with the old parser and rewrites it in this format.
 *
 * 2 (M7): node_entry_t gained a trailing desired_mode byte (see
 * swarm_power_mode_t below). swarm_store.c migrates a format-1 blob in
 * place, same shape as the format-0 (M5a) migration -- see load_nodes_blob(). */
#define SWARM_STORE_FORMAT 2

/* Power mode a battery node runs under (M7). ALWAYS_ON never sleeps and is
 * the default for both a brand-new own-device mode and an unknown node's
 * desired mode -- a node/hub that has never heard otherwise behaves exactly
 * like a pre-M7 device. BATTERY_15/60 are the two checkin-interval presets;
 * see batt_cycle.h's batt_period_s() for the actual seconds each maps to. */
typedef enum {
    SWARM_PM_ALWAYS_ON  = 0,
    SWARM_PM_BATTERY_15 = 1,
    SWARM_PM_BATTERY_60 = 2,
} swarm_power_mode_t;
#define SWARM_PM_VALID(m) ((m) <= SWARM_PM_BATTERY_60)

/* Node display name: up to this many bytes, NUL-terminated in the buffer
 * callers pass to swarm_store_node_name(). Empty means unset. */
#define SWARM_NODE_NAME_LEN 24

typedef enum {
    SWARM_ROLE_UNSET = 0,
    SWARM_ROLE_MAIN  = 1,
    SWARM_ROLE_NODE  = 2,
} swarm_role_t;

/* Loads role + peer state from NVS (namespace "planthub") into an in-RAM
 * copy guarded by a mutex, so the ESP-NOW receive path never touches NVS. */
esp_err_t swarm_store_init(void);

swarm_role_t swarm_store_role(void);
esp_err_t swarm_store_set_role(swarm_role_t r);

/* Node side: the hub this node is paired to. Returns false when unpaired. */
bool swarm_store_hub(uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN], uint8_t *channel_out);
esp_err_t swarm_store_set_hub(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN], uint8_t channel);
esp_err_t swarm_store_set_channel(uint8_t channel);
esp_err_t swarm_store_clear_hub(void);

/* Node side: the regulatory-domain country the hub reported in its most
 * recent PAIR_ACK (protocol v2+, PlanV1 3.3 country inheritance), applied
 * via esp_wifi_set_country_code() before any channel sweep and persisted
 * here so a reboot doesn't lose it -- falling back to the compile-time
 * default would silently re-restrict a node back to channels 1-11
 * mid-deployment. Stored under its own NVS key, deliberately NOT folded
 * into the hub blob above: that keeps the hub blob's on-disk layout
 * byte-for-byte unchanged, so a device already paired under protocol v1
 * keeps that pairing across the upgrade instead of needing a migration of
 * its own (contrast the node table's migration in swarm_store.c, needed
 * precisely because SWARM_MAX_NODES changed that blob's length).
 * swarm_store_hub_country() returns false (out untouched) when nothing
 * has been learned yet -- a node that has never paired under v2, or one
 * that was just factory-reset, then keeps the compile-time default
 * (CONFIG_PLANTHUB_WIFI_COUNTRY). */
bool swarm_store_hub_country(char out[3]);
esp_err_t swarm_store_set_hub_country(const char cc[3]);

/* Operator-selected WiFi region (M9), independent of the hub_country pair
 * above and of CONFIG_PLANTHUB_WIFI_COUNTRY. Set on THIS device (hub or
 * node) via POST /api/v1/config's "region" field; espnow_link.c's country
 * setup gives it top precedence over everything else -- an explicit
 * operator choice beats router hearsay (the hub's 802.11d auto-adoption)
 * and beats a node's learned sw_hubcc, and unlike both of those it forces
 * MANUAL policy (no 802.11d) even on the hub. See espnow_link.c for the
 * full precedence chain and why.
 *
 * Only the four codes the webui's "WiFi region" select offers are ever
 * accepted: "CH" (Europe, channels 1-13), "US" (1-11), "JP" (1-14), "01"
 * (IDF's world-safe default, 1-11) -- swarm_store_set_region() rejects
 * anything else with ESP_ERR_INVALID_ARG, and this validation is
 * deliberately NOT the general-purpose "any two bytes" flexibility
 * swarm_store_set_hub_country() allows (that one only ever receives an
 * already-validated code echoed back from another PlantHub device's own
 * esp_wifi_get_country(), never arbitrary user input over HTTP).
 *
 * Stored under its own NVS key, same "false = nothing set, caller falls
 * back to the next thing in the precedence chain" shape as
 * swarm_store_hub_country() above. swarm_store_set_region("") clears back
 * to unset (also done by swarm_store_reset_all(), AND by api_v1.c's
 * role_post() on every successful role change -- a region set from a
 * device's PREVIOUS role must not survive into its next one: at TOP
 * precedence, a stale region would permanently outrank the country a
 * newly-converted node is supposed to LEARN from its new hub's PAIR_ACK,
 * silently defeating pairing if the new hub's channel falls outside the
 * stale region's allowed range. Node regions are meant to come from
 * whatever hub the device actually pairs with next, not from a role it no
 * longer has). Applies at next boot only, like the rest of
 * /api/v1/config -- there is no live re-init of the radio's country here.
 *
 * swarm_store_set_region() takes a plain NUL-terminated C string (unlike
 * swarm_store_hub_country()'s fixed char[3] pair above) because its real
 * caller hands over a cJSON string of whatever length untrusted HTTP JSON
 * contained -- the function itself checks that length (strlen(cc) <= 2)
 * as part of validation, rather than trusting a fixed-size buffer the
 * caller already sized correctly. */
bool swarm_store_region(char out[3]);
esp_err_t swarm_store_set_region(const char *cc);

/* Node side (M7): this device's OWN power mode + battery bookkeeping.
 * Deliberately separate NVS keys from KEY_HUB/KEY_NODES -- this is state
 * about the device itself, not about its pairing to a hub, so it survives
 * independently and is what node_ota.c/node_ota_recv.c's checkin path
 * (Task 5) reads/writes every wake. Defaults when nothing is stored yet:
 * ALWAYS_ON, 0 failed wakes, 0 wake_counter -- a node that has never set
 * any of these behaves exactly like a pre-M7 device. set_power_mode()
 * rejects a value SWARM_PM_VALID() disagrees with (ESP_ERR_INVALID_ARG). */
swarm_power_mode_t swarm_store_power_mode(void);
esp_err_t swarm_store_set_power_mode(swarm_power_mode_t m);
uint32_t  swarm_store_failed_wakes(void);
esp_err_t swarm_store_set_failed_wakes(uint32_t n);
uint32_t  swarm_store_wake_counter(void);
esp_err_t swarm_store_set_wake_counter(uint32_t n);

/* Hub side: paired nodes (single node in M5a; table grows to SWARM_MAX_NODES
 * in M5b). */
int swarm_store_node_count(void);
bool swarm_store_node_at(int idx, uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN]);
esp_err_t swarm_store_add_node(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN]);
esp_err_t swarm_store_clear_nodes(void);

/* Hub side: operator-assigned node name, stored alongside the node's table
 * entry. name may be NULL or "" to clear it; anything longer than
 * SWARM_NODE_NAME_LEN bytes is rejected with ESP_ERR_INVALID_SIZE.
 * ESP_ERR_NOT_FOUND if mac isn't in the table. */
esp_err_t swarm_store_set_node_name(const uint8_t mac[6], const char *name);

/* Fills out (a buffer of at least SWARM_NODE_NAME_LEN+1 bytes) with the
 * node's stored name, NUL-terminated -- "" when unset. Returns false only
 * when mac isn't in the table at all (out is left untouched in that case). */
bool swarm_store_node_name(const uint8_t mac[6], char out[SWARM_NODE_NAME_LEN + 1]);

/* Removes mac from the persisted node table (compacting the array). The
 * caller is responsible for also removing the corresponding ESP-NOW peer
 * (espnow_link_remove_peer()) -- this function only touches swarm_store's
 * own state. ESP_ERR_NOT_FOUND if mac isn't in the table. */
esp_err_t swarm_store_forget_node(const uint8_t mac[6]);

/* Hub side (M7): per-node desired power mode, persisted as part of the
 * node's table entry (node_entry_t.desired_mode, format 2 -- see
 * SWARM_STORE_FORMAT above). This is what the hub's checkin-ack path
 * (Task 6) reconciles a node's reported mode against. The getter returns
 * SWARM_PM_ALWAYS_ON for a mac not in the table, matching a freshly-added
 * node's default (mirrors swarm_store_node_name()'s "" default -- an
 * unknown/never-set node behaves like a pre-M7 always-on one). The setter
 * rejects a value SWARM_PM_VALID() disagrees with (ESP_ERR_INVALID_ARG)
 * and returns ESP_ERR_NOT_FOUND if mac isn't paired. */
swarm_power_mode_t swarm_store_node_desired_mode(const uint8_t mac[6]);
esp_err_t swarm_store_set_node_desired_mode(const uint8_t mac[6], swarm_power_mode_t m);

/* Node side: true once a pairing search (pairing_node_start(), driven by
 * swarm_start_node_search()) has run to completion and failed/timed out.
 * An unpaired node checks this at boot to decide whether to actively sweep
 * for a hub again (flag clear) or sit in the portal so a human can see the
 * failure and retry via POST /api/v1/pair/retry (flag set). Cleared on a
 * successful pairing and by a factory reset. */
bool      swarm_store_pair_failed(void);
esp_err_t swarm_store_set_pair_failed(bool failed);

/* Factory reset: returns this device to a fresh, role-unset state --
 * clears role, the stored hub peer, the node table, the pair-failed flag,
 * (M7) this device's own power mode + battery bookkeeping (mode,
 * failed_wakes, wake_counter -- all back to their ALWAYS_ON/0/0 defaults),
 * and (M9) the operator-selected WiFi region (back to unset). Used by
 * claim.c's physical reset button so a node (which may run no web server
 * at all once paired) is always recoverable without reflashing. */
esp_err_t swarm_store_reset_all(void);

/* Node side: identifies the most recently node_ota_recv.c-completed OTA
 * session, i.e. one whose esp_ota_set_boot_partition() has already
 * succeeded -- the update is genuinely committed, not merely in progress.
 * See node_ota_recv.c's finalize_session() for the writer and
 * handle_begin()/handle_chunk() for the readers.
 *
 * WHY THIS EXISTS: a node OTA session finishes with
 * esp_ota_set_boot_partition() immediately followed by esp_restart() --
 * that reboot wipes the RAM-only session state (node_ota_recv.c's
 * recv_session_t) that would otherwise let a later OTA_CHUNK/OTA_BEGIN be
 * recognised as "this session already finished, right here" rather than
 * "no session at all". node_ota_recv.c's own DONE-retransmission (sent 5x
 * before the restart) is the fast path for telling the hub that; this is
 * the backstop for when every one of those is also lost -- M5c hardware
 * round 6 observed exactly that: the hub's acked_offset lagged reality so
 * badly (broadcast OTA_STATUS is lossy by design, see swarm_frame.h) that
 * it was still mid-drain, not yet listening for a terminal status, when
 * this node rebooted. Without this, the node's only truthful answer to the
 * hub's continued traffic was OTA_ST_IDLE ("no active session"), which the
 * hub cannot tell apart from "I never had this session" -- and reports the
 * update FAILED even though it fully succeeded.
 *
 * SAFETY even though this is a bare id+length with no expiry timer: the
 * session_id is a fresh esp_random() value (swarm_frame.h's
 * swarm_ota_begin_t comment), and node_ota.c's node_ota_handle_status()
 * only ever credits a non-IDLE OTA_STATUS to a hub-side session whose OWN
 * session_id matches the one echoed back. So even if this marker were
 * somehow echoed to an unrelated later session (it won't be under normal
 * operation -- see swarm_store_clear_completed_ota_session()), a mismatched
 * id makes the hub silently ignore it, same as no reply at all. That
 * built-in mismatch-is-harmless property is what lets this stay simple: no
 * separate expiry clock, no hub-MAC cross-check duplicating what
 * node_ota_recv.c's from_stored_hub() already gates at the enqueue
 * boundary.
 *
 * Returns false (outputs untouched) when nothing is stored. */
bool swarm_store_completed_ota_session(uint32_t *session_id_out, uint32_t *total_len_out);
esp_err_t swarm_store_set_completed_ota_session(uint32_t session_id, uint32_t total_len);

/* Drops the marker above. Called once a genuinely NEW OTA_BEGIN (different
 * session_id) is accepted (node_ota_recv.c's handle_begin()) -- from that
 * point on, the OLD session's hub has either already gotten its answer or
 * long since given up, and nothing later this boot should still be able to
 * match its id. Also called by swarm_store_reset_all() (factory reset). */
esp_err_t swarm_store_clear_completed_ota_session(void);
