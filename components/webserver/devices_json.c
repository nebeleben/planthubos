#include "devices_json.h"
#include "app_config.h"
#include "swarm_store.h"
#include "capability.h"
#include "plants.h"
#include "bthome.h"
#include <stdio.h>

/* now_uptime_s - last_seen_s, both esp_timer uptime seconds off the same
 * monotonic clock -- floor-at-zero clamp, same defensive spirit as
 * swarm.c's identical age conversion. */
static uint32_t age_s(uint32_t now_uptime_s, uint32_t last_seen_s)
{
    return (now_uptime_s >= last_seen_s) ? (now_uptime_s - last_seen_s) : 0;
}

static const char *device_kind_str(uint8_t kind)
{
    switch ((device_kind_t)kind) {
    case DEV_KIND_BLE:    return "ble";
    case DEV_KIND_ESPNOW: return "espnow";
    case DEV_KIND_ZIGBEE: return "zb";
    }
    return "?";
}

static void via_node_mac_str(char *buf, size_t buflen, const uint8_t mac[6])
{
    snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Every plant that currently binds ANY capability of `id` (plants_table.h's
 * per-capability cap_bound[]/cap_dev[]), added to `arr` once each even when
 * a plant binds more than one of this device's capabilities. */
static void add_plant_ids(cJSON *arr, const plants_table_t *plants, const device_id_t *id)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *p = &plants->p[i];
        if (!p->in_use) continue;
        bool bound = false;
        for (uint8_t c = 0; c < CAPABILITY_COUNT && !bound; c++) {
            if (p->cap_bound[c] && device_id_equal(&p->cap_dev[c], id)) bound = true;
        }
        if (bound) cJSON_AddItemToArray(arr, cJSON_CreateNumber(p->id));
    }
}

cJSON *device_json(const device_entry_t *e, const plants_table_t *plants, uint32_t now_uptime_s)
{
    cJSON *o = cJSON_CreateObject();

    char idbuf[24];
    device_id_format(&e->id, idbuf, sizeof(idbuf));
    cJSON_AddStringToObject(o, "id", idbuf);
    cJSON_AddStringToObject(o, "kind", device_kind_str(e->id.kind));
    cJSON_AddNumberToObject(o, "last_seen_s", age_s(now_uptime_s, e->last_seen_s));
    /* M3 Task 7 (spec §4): "Keys are never returned by any GET -- the API
     * reports only has_key: true|false." bindkey_has() takes the SAME
     * dev_id string device_id_format() just built (bindkey.c's own
     * contract), so this is a cheap, well-defined check for every device
     * kind -- not just BLE/BTHome -- even though only BLE devices can
     * currently have a key set via POST /api/v1/devices/{id}/key. */
    cJSON_AddBoolToObject(o, "has_key", bindkey_has(idbuf));

    if (e->via_node_valid) {
        char node_name[SWARM_NODE_NAME_LEN + 1];
        if (swarm_store_node_name(e->via_node, node_name) && node_name[0] != '\0') {
            cJSON_AddStringToObject(o, "via", node_name);
        } else {
            char macbuf[18];
            via_node_mac_str(macbuf, sizeof(macbuf), e->via_node);
            cJSON_AddStringToObject(o, "via", macbuf);
        }
    } else {
        cJSON_AddNullToObject(o, "via");
    }
    cJSON_AddNumberToObject(o, "rssi", e->best_rssi);

    /* Sensor display names are still mac-keyed in app_config (V1 heritage;
     * generalising the name store to device_id_t is out of this task's
     * scope, see task-6-report.md), so only BLE devices -- whose addr IS a
     * mac -- can have one. */
    char name[33];
    if (e->id.kind == DEV_KIND_BLE && app_config_get_sensor_name(e->id.addr, name) && name[0] != '\0') {
        cJSON_AddStringToObject(o, "name", name);
    } else {
        cJSON_AddNullToObject(o, "name");
    }

    cJSON *caps = cJSON_AddArrayToObject(o, "caps");
    for (uint8_t c = 0; c < CAPABILITY_COUNT; c++) {
        if (!e->caps[c].valid) continue;
        const capability_t *cap = capability_get(c);
        if (!cap) continue;
        cJSON *co = cJSON_CreateObject();
        cJSON_AddNumberToObject(co, "id", c);
        cJSON_AddStringToObject(co, "name", cap->name);
        cJSON_AddStringToObject(co, "unit", cap->unit);
        cJSON_AddNumberToObject(co, "value", capability_decode(c, e->caps[c].raw));
        cJSON_AddNumberToObject(co, "age_s", age_s(now_uptime_s, e->caps[c].updated_s));
        cJSON_AddItemToArray(caps, co);
    }

    cJSON *plant_ids = cJSON_AddArrayToObject(o, "plant_ids");
    if (plants) add_plant_ids(plant_ids, plants, &e->id);

    return o;
}

cJSON *plant_json(const plant_entry_t *p, const registry_t *reg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);   /* "" = unnamed; UI renders "Plant <id>" */

    cJSON *bindings = cJSON_AddArrayToObject(o, "bindings");
    for (uint8_t c = 0; c < CAPABILITY_COUNT; c++) {
        if (!p->cap_bound[c]) continue;
        const capability_t *cap = capability_get(c);

        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "cap", c);
        cJSON_AddStringToObject(b, "name", cap ? cap->name : "?");
        char devbuf[24];
        device_id_format(&p->cap_dev[c], devbuf, sizeof(devbuf));
        cJSON_AddStringToObject(b, "device", devbuf);

        float value; uint32_t age_s;
        if (plants_cap_value(p->id, c, reg, &value, &age_s)) {
            cJSON_AddNumberToObject(b, "value", value);
            cJSON_AddNumberToObject(b, "age_s", age_s);
        } else {
            cJSON_AddNullToObject(b, "value");
            cJSON_AddNullToObject(b, "age_s");
        }
        cJSON_AddItemToArray(bindings, b);
    }
    return o;
}
