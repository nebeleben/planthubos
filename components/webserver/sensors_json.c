#include "sensors_json.h"
#include "app_config.h"
#include "swarm_store.h"
#include <stdio.h>

cJSON *sensor_json(const sensor_entry_t *e)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mac", mac);
    char name[33];
    if (app_config_get_sensor_name(e->mac, name)) cJSON_AddStringToObject(o, "name", name);
    else cJSON_AddNullToObject(o, "name");
    if (e->latest.has_temp) cJSON_AddNumberToObject(o, "temp", e->latest.temp_dc / 10.0);
    else cJSON_AddNullToObject(o, "temp");
    if (e->latest.has_moisture) cJSON_AddNumberToObject(o, "moisture", e->latest.moisture_pct);
    else cJSON_AddNullToObject(o, "moisture");
    if (e->latest.has_lux) cJSON_AddNumberToObject(o, "lux", e->latest.lux);
    else cJSON_AddNullToObject(o, "lux");
    if (e->latest.has_conductivity) cJSON_AddNumberToObject(o, "conductivity", e->latest.conductivity_us);
    else cJSON_AddNullToObject(o, "conductivity");
    if (e->latest.has_battery) cJSON_AddNumberToObject(o, "battery", e->latest.battery_pct);
    else cJSON_AddNullToObject(o, "battery");
    cJSON_AddNumberToObject(o, "last_seen_s", e->last_seen_s);

    /* "via" (M5b attribution): null means the hub heard this sensor
     * directly on its own BLE radio -- never relayed. Non-null identifies
     * the node registry_update_from()'s "strongest rssi wins" rule
     * currently attributes it to (see registry.h), so moving a plant
     * closer to a different node re-attributes it automatically and that
     * becomes visible here without any election protocol. */
    if (e->via_node_valid) {
        cJSON *via = cJSON_CreateObject();
        char via_mac[18];
        snprintf(via_mac, sizeof(via_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 e->via_node[0], e->via_node[1], e->via_node[2],
                 e->via_node[3], e->via_node[4], e->via_node[5]);
        cJSON_AddStringToObject(via, "mac", via_mac);
        char node_name[SWARM_NODE_NAME_LEN + 1];
        if (swarm_store_node_name(e->via_node, node_name) && node_name[0] != '\0') {
            cJSON_AddStringToObject(via, "name", node_name);
        } else {
            cJSON_AddNullToObject(via, "name");
        }
        cJSON_AddNumberToObject(via, "rssi", e->best_rssi);
        cJSON_AddItemToObject(o, "via", via);
    } else {
        cJSON_AddNullToObject(o, "via");
    }
    return o;
}
