#include "sensors_json.h"
#include "app_config.h"
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
    return o;
}
