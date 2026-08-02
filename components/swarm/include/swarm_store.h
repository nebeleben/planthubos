#pragma once
#include "esp_err.h"
#include "swarm_frame.h"
#include <stdbool.h>
#include <stdint.h>

#define SWARM_MAX_NODES 8  /* was 4 in M5a */

/* On-disk format of the persisted node table (KEY_NODES blob). Bump this
 * and add a migration branch in swarm_store.c's load whenever the layout
 * changes again -- M5a accepted a stored blob on exact-length match only,
 * which is why simply raising SWARM_MAX_NODES here would otherwise have
 * silently wiped every already-paired node's table on upgrade (a longer
 * fixed-size array changes the blob's length). See swarm_store.c for the
 * migration that reads an M5a-format blob (no format byte, exact old
 * length) with the old parser and rewrites it in this format. */
#define SWARM_STORE_FORMAT 1

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

/* Node side: true once a pairing search (pairing_node_start(), driven by
 * swarm_start_node_search()) has run to completion and failed/timed out.
 * An unpaired node checks this at boot to decide whether to actively sweep
 * for a hub again (flag clear) or sit in the portal so a human can see the
 * failure and retry via POST /api/v1/pair/retry (flag set). Cleared on a
 * successful pairing and by a factory reset. */
bool      swarm_store_pair_failed(void);
esp_err_t swarm_store_set_pair_failed(bool failed);

/* Factory reset: returns this device to a fresh, role-unset state --
 * clears role, the stored hub peer, the node table and the pair-failed
 * flag. Used by claim.c's physical reset button so a node (which may run
 * no web server at all once paired) is always recoverable without
 * reflashing. */
esp_err_t swarm_store_reset_all(void);
