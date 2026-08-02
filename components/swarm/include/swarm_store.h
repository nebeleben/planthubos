#pragma once
#include "esp_err.h"
#include "swarm_frame.h"
#include <stdbool.h>
#include <stdint.h>

#define SWARM_MAX_NODES 4  /* M5a; raised in M5b */

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

/* Hub side: paired nodes (single node in M5a; table grows in M5b). */
int swarm_store_node_count(void);
bool swarm_store_node_at(int idx, uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN]);
esp_err_t swarm_store_add_node(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN]);
esp_err_t swarm_store_clear_nodes(void);

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
