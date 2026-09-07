#include "swarm_rules.h"
#include <stddef.h>

bool swarm_rules_node_radio_ok(radio_role_t role, swarm_power_mode_t power_mode, const char **why)
{
    const char *r = NULL;
    bool ok = true;
    if (role == RADIO_ROLE_WIFI_ONLY) { ok = false; r = "a node relays sensors; it needs BLE or Zigbee"; }
    else if (role == RADIO_ROLE_ZIGBEE && power_mode != SWARM_PM_ALWAYS_ON) { ok = false; r = "a Zigbee bridge cannot sleep: set the node to always-on first"; }
    else if ((int)role < 0 || (int)role > (int)RADIO_ROLE_ZIGBEE) { ok = false; r = "unknown radio role"; }
    if (why) *why = r;
    return ok;
}

bool swarm_rules_node_power_ok(swarm_power_mode_t power_mode, radio_role_t role, const char **why)
{
    const char *r = NULL;
    bool ok = true;
    if (!SWARM_PM_VALID(power_mode)) { ok = false; r = "unknown power mode"; }
    else if (power_mode != SWARM_PM_ALWAYS_ON && role == RADIO_ROLE_ZIGBEE) { ok = false; r = "a Zigbee bridge cannot sleep: switch its radio to BLE first"; }
    if (why) *why = r;
    return ok;
}
