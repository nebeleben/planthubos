#pragma once
#include <stdbool.h>
#include "radio_role_str.h"
#include "swarm_power_mode.h"   /* swarm_power_mode_t; IDF-free, unlike swarm_store.h */

/* Node-only compatibility rules, pure so they are host-tested. A node has
 * no web UI, so every refusal reason is a sentence the hub can show. */
bool swarm_rules_node_radio_ok(radio_role_t role, swarm_power_mode_t power_mode, const char **why);
bool swarm_rules_node_power_ok(swarm_power_mode_t power_mode, radio_role_t role, const char **why);
