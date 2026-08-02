#include "registry.h"
#include <string.h>

void registry_init(registry_t *r) { memset(r, 0, sizeof(*r)); }

int registry_find(const registry_t *r, const uint8_t mac[6])
{
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++)
        if (r->sensors[i].in_use && memcmp(r->sensors[i].mac, mac, 6) == 0) return i;
    return -1;
}

int registry_count(const registry_t *r)
{
    int n = 0;
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) n += r->sensors[i].in_use;
    return n;
}

static void merge(mibeacon_t *dst, const mibeacon_t *src)
{
    dst->product_id = src->product_id;
    dst->frame_cnt = src->frame_cnt;
    if (src->has_temp)         { dst->temp_dc = src->temp_dc;                 dst->has_temp = true; }
    if (src->has_moisture)     { dst->moisture_pct = src->moisture_pct;       dst->has_moisture = true; }
    if (src->has_lux)          { dst->lux = src->lux;                         dst->has_lux = true; }
    if (src->has_conductivity) { dst->conductivity_us = src->conductivity_us; dst->has_conductivity = true; }
    if (src->has_battery)      { dst->battery_pct = src->battery_pct;         dst->has_battery = true; }
}

static void set_attribution(sensor_entry_t *e, const uint8_t via_node[6], int8_t rssi, uint32_t now_s)
{
    if (via_node) {
        e->via_node_valid = true;
        memcpy(e->via_node, via_node, 6);
    } else {
        e->via_node_valid = false;
        memset(e->via_node, 0, 6);
    }
    e->best_rssi = rssi;
    e->attributed_s = now_s;
}

int registry_update_from(registry_t *r, const mibeacon_t *m, uint32_t now_s,
                          const uint8_t via_node[6], int8_t rssi)
{
    int idx = registry_find(r, m->mac);
    if (idx < 0) {
        for (int i = 0; i < REGISTRY_MAX_SENSORS && idx < 0; i++)
            if (!r->sensors[i].in_use) idx = i;
        if (idx < 0) return -1;
        sensor_entry_t *e = &r->sensors[idx];
        memset(e, 0, sizeof(*e));
        e->in_use = true;
        memcpy(e->mac, m->mac, 6);
        merge(&e->latest, m);
        e->last_seen_s = now_s;
        set_attribution(e, via_node, rssi, now_s);
        return 1;
    }
    sensor_entry_t *e = &r->sensors[idx];
    e->last_seen_s = now_s;
    if (e->latest.frame_cnt == m->frame_cnt) {
        /* Duplicate frame: attribution can still hand off to a stronger
         * reporter -- or unconditionally to a direct reception, which
         * always outranks a relay -- without this being counted as new
         * data. Only a fresh frame_cnt (below) resets the contest. */
        if (via_node == NULL) {
            set_attribution(e, NULL, rssi, now_s);
        } else if (e->via_node_valid && rssi > e->best_rssi) {
            /* Only contest attribution against another node's rssi; if the
             * hub already attributed this frame to itself directly, that
             * stands regardless of what any node claims. */
            set_attribution(e, via_node, rssi, now_s);
        }
        return 0;   /* repeated advertisement */
    }
    merge(&e->latest, m);
    set_attribution(e, via_node, rssi, now_s);
    return 1;
}

int registry_update(registry_t *r, const mibeacon_t *m, uint32_t now_s)
{
    return registry_update_from(r, m, now_s, NULL, 0);
}
