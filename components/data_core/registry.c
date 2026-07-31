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

int registry_update(registry_t *r, const mibeacon_t *m, uint32_t now_s)
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
        return 1;
    }
    sensor_entry_t *e = &r->sensors[idx];
    e->last_seen_s = now_s;
    if (e->latest.frame_cnt == m->frame_cnt) return 0;   /* repeated advertisement */
    merge(&e->latest, m);
    return 1;
}
