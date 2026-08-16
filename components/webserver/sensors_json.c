#include "sensors_json.h"
#include "app_config.h"
#include "swarm_store.h"
#include "plants.h"
#include "storage.h"
#include <stdio.h>

/* ---------------- shared field helpers ---------------- */

/* {temp, moisture, lux, conductivity} straight off a mibeacon_t's has_x /
 * x value pairs -- the reading fields every "live registry entry" shape
 * (sensor_json(), plant_json()'s live branch) carries identically. */
static void add_reading_fields(cJSON *o, const mibeacon_t *m)
{
    if (m->has_temp) cJSON_AddNumberToObject(o, "temp", m->temp_dc / 10.0);
    else cJSON_AddNullToObject(o, "temp");
    if (m->has_moisture) cJSON_AddNumberToObject(o, "moisture", m->moisture_pct);
    else cJSON_AddNullToObject(o, "moisture");
    if (m->has_lux) cJSON_AddNumberToObject(o, "lux", m->lux);
    else cJSON_AddNullToObject(o, "lux");
    if (m->has_conductivity) cJSON_AddNumberToObject(o, "conductivity", m->conductivity_us);
    else cJSON_AddNullToObject(o, "conductivity");
}

static void add_mac_str(cJSON *o, const char *key, const uint8_t mac[6])
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, key, buf);
}

/* {mac, name, rssi} of the node currently attributed as `e`'s source (M5b) --
 * shared by sensor_json()'s "via" and probe_core_json()'s "via" below. Only
 * call when e->via_node_valid. */
static cJSON *via_json(const sensor_entry_t *e)
{
    cJSON *via = cJSON_CreateObject();
    add_mac_str(via, "mac", e->via_node);
    char node_name[SWARM_NODE_NAME_LEN + 1];
    if (swarm_store_node_name(e->via_node, node_name) && node_name[0] != '\0') {
        cJSON_AddStringToObject(via, "name", node_name);
    } else {
        cJSON_AddNullToObject(via, "name");
    }
    cJSON_AddNumberToObject(via, "rssi", e->best_rssi);
    return via;
}

/* {mac, battery, rssi, via} for a currently-live registry entry -- the
 * probe diagnostics bundle shared by probe_json() (which adds
 * last_seen_s/plant_id on top) and plant_json()'s nested "probe" object for
 * an assigned, currently-reporting sensor. */
static cJSON *probe_core_json(const sensor_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    add_mac_str(o, "mac", e->mac);
    if (e->latest.has_battery) cJSON_AddNumberToObject(o, "battery", e->latest.battery_pct);
    else cJSON_AddNullToObject(o, "battery");
    cJSON_AddNumberToObject(o, "rssi", e->best_rssi);
    if (e->via_node_valid) cJSON_AddItemToObject(o, "via", via_json(e));
    else cJSON_AddNullToObject(o, "via");
    return o;
}

/* now_uptime_s - last_seen_s, both esp_timer uptime seconds off the same
 * monotonic clock -- floor-at-zero clamp, same defensive spirit as
 * swarm.c's identical age conversion (node_ota.c's next_offset clamp is the
 * same pattern again). */
static uint32_t age_s(uint32_t now_uptime_s, uint32_t last_seen_s)
{
    return (now_uptime_s >= last_seen_s) ? (now_uptime_s - last_seen_s) : 0;
}

/* ---------------- sensor_json() -- SSE's full shape, unchanged ---------------- */

cJSON *sensor_json(const sensor_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    add_mac_str(o, "mac", e->mac);
    char name[33];
    if (app_config_get_sensor_name(e->mac, name)) cJSON_AddStringToObject(o, "name", name);
    else cJSON_AddNullToObject(o, "name");
    add_reading_fields(o, &e->latest);
    if (e->latest.has_battery) cJSON_AddNumberToObject(o, "battery", e->latest.battery_pct);
    else cJSON_AddNullToObject(o, "battery");
    cJSON_AddNumberToObject(o, "last_seen_s", e->last_seen_s);
    if (e->via_node_valid) cJSON_AddItemToObject(o, "via", via_json(e));
    else cJSON_AddNullToObject(o, "via");
    return o;
}

/* ---------------- probe_json() -- demoted GET /api/v1/sensors ---------------- */

cJSON *probe_json(const sensor_entry_t *e, uint8_t plant_id, uint32_t now_uptime_s)
{
    cJSON *o = probe_core_json(e);
    char name[33];
    if (app_config_get_sensor_name(e->mac, name)) cJSON_AddStringToObject(o, "name", name);
    else cJSON_AddNullToObject(o, "name");
    cJSON_AddNumberToObject(o, "last_seen_s", age_s(now_uptime_s, e->last_seen_s));
    if (plant_id != 0) cJSON_AddNumberToObject(o, "plant_id", plant_id);
    else cJSON_AddNullToObject(o, "plant_id");
    return o;
}

/* ---------------- plant_json() -- GET /api/v1/plants ---------------- */

cJSON *plant_json(const plant_entry_t *p, const legacy_registry_t *snap,   /* M2-SHIM */
                   uint32_t now_uptime_s, uint32_t now_epoch)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);   /* "" = unnamed; UI renders "Plant <id>" */

    int idx = p->mac_valid ? legacy_registry_find(snap, p->mac) : -1;   /* M2-SHIM */
    if (idx >= 0) {
        /* Live: assigned mac, currently in the registry (heard at least
         * once this boot). */
        const sensor_entry_t *e = &snap->sensors[idx];
        add_reading_fields(o, &e->latest);
        if (e->latest.has_battery) cJSON_AddNumberToObject(o, "battery", e->latest.battery_pct);
        else cJSON_AddNullToObject(o, "battery");
        cJSON_AddNumberToObject(o, "last_seen_s", age_s(now_uptime_s, e->last_seen_s));
        cJSON_AddItemToObject(o, "probe", probe_core_json(e));
        return o;
    }

    /* Probe-less, OR assigned to a mac the radio has never actually heard
     * yet (the probe route's "pre-assign a replacement probe" case, see
     * api_v1.c's plants_probe_post comment) -- either way there is no live
     * registry entry, so fall back to the plant's last known history
     * record (spec §2's "keeps showing last known values, nothing is
     * blanked"). This is a judgement call for the pending-probe sub-case
     * (arguably "no data yet" could read as all-null instead of the
     * PREVIOUS probe's last values) -- documented here rather than
     * silently chosen: showing stale-but-real data until the new probe's
     * first reading arrives is more useful than blanking a plant that
     * still has a history. */
    int16_t temp_dc; uint8_t moisture_pct, battery_pct; uint32_t lux, epoch; uint16_t cond;
    bool found = plants_last_values(p->id, &temp_dc, &moisture_pct, &lux, &cond, &battery_pct, &epoch);
    if (found) {
        if (temp_dc != STORAGE_TEMP_NONE) cJSON_AddNumberToObject(o, "temp", temp_dc / 10.0);
        else cJSON_AddNullToObject(o, "temp");
        if (moisture_pct != STORAGE_U8_NONE) cJSON_AddNumberToObject(o, "moisture", moisture_pct);
        else cJSON_AddNullToObject(o, "moisture");
        if (lux != STORAGE_LUX_NONE) cJSON_AddNumberToObject(o, "lux", lux);
        else cJSON_AddNullToObject(o, "lux");
        if (cond != STORAGE_U16_NONE) cJSON_AddNumberToObject(o, "conductivity", cond);
        else cJSON_AddNullToObject(o, "conductivity");
        if (battery_pct != STORAGE_U8_NONE) cJSON_AddNumberToObject(o, "battery", battery_pct);
        else cJSON_AddNullToObject(o, "battery");
        cJSON_AddNumberToObject(o, "last_seen_s", age_s(now_epoch, epoch));
    } else {
        cJSON_AddNullToObject(o, "temp");
        cJSON_AddNullToObject(o, "moisture");
        cJSON_AddNullToObject(o, "lux");
        cJSON_AddNullToObject(o, "conductivity");
        cJSON_AddNullToObject(o, "battery");
        cJSON_AddNullToObject(o, "last_seen_s");
    }

    if (p->mac_valid) {
        /* Pending: assigned but never heard -- show the target mac with
         * nulled-out live diagnostics rather than pretending it's fully
         * unassigned. */
        cJSON *probe = cJSON_CreateObject();
        add_mac_str(probe, "mac", p->mac);
        cJSON_AddNullToObject(probe, "battery");
        cJSON_AddNullToObject(probe, "rssi");
        cJSON_AddNullToObject(probe, "via");
        cJSON_AddItemToObject(o, "probe", probe);
    } else {
        cJSON_AddNullToObject(o, "probe");
    }
    return o;
}
