#include <assert.h>
#include <stdio.h>
#include "swarm_rules.h"

int main(void)
{
    const char *why = NULL;
    assert(swarm_rules_node_radio_ok(RADIO_ROLE_BLE, SWARM_PM_ALWAYS_ON, &why));
    assert(swarm_rules_node_radio_ok(RADIO_ROLE_BLE, SWARM_PM_BATTERY_60, &why));
    assert(swarm_rules_node_radio_ok(RADIO_ROLE_ZIGBEE, SWARM_PM_ALWAYS_ON, &why));
    assert(!swarm_rules_node_radio_ok(RADIO_ROLE_ZIGBEE, SWARM_PM_BATTERY_15, &why) && why);
    assert(!swarm_rules_node_radio_ok(RADIO_ROLE_WIFI_ONLY, SWARM_PM_ALWAYS_ON, &why) && why);
    assert(!swarm_rules_node_radio_ok((radio_role_t)9, SWARM_PM_ALWAYS_ON, &why));
    assert(swarm_rules_node_power_ok(SWARM_PM_BATTERY_15, RADIO_ROLE_BLE, &why));
    assert(!swarm_rules_node_power_ok(SWARM_PM_BATTERY_15, RADIO_ROLE_ZIGBEE, &why) && why);
    assert(swarm_rules_node_power_ok(SWARM_PM_ALWAYS_ON, RADIO_ROLE_ZIGBEE, &why));
    printf("test_swarm_rules: OK\n");
    return 0;
}
