#include "registry.h"
#include <string.h>

void registry_init(registry_t *r) { memset(r, 0, sizeof(*r)); }

int registry_find(const registry_t *r, const device_id_t *id)
{
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++)
        if (r->devices[i].in_use && device_id_equal(&r->devices[i].id, id)) return i;
    return -1;
}

int registry_count(const registry_t *r)
{
    int n = 0;
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) n += r->devices[i].in_use;
    return n;
}

/* Finds the device by id, or claims the first free slot and initialises it
 * (in_use, id, every cap slot cleared to CAP_VALUE_NONE/invalid) when
 * unknown. -1 when unknown and the table is full -- nothing is created or
 * modified in that case. *created reports which path was taken (NULL if the
 * caller doesn't need to know). */
static int find_or_create(registry_t *r, const device_id_t *id, bool *created)
{
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        if (r->devices[i].in_use && device_id_equal(&r->devices[i].id, id)) {
            if (created) *created = false;
            return i;
        }
    }
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        if (r->devices[i].in_use) continue;
        device_entry_t *d = &r->devices[i];
        memset(d, 0, sizeof(*d));
        d->in_use = true;
        d->id = *id;
        for (int c = 0; c < CAPABILITY_COUNT; c++) d->caps[c].raw = CAP_VALUE_NONE;
        if (created) *created = true;
        return i;
    }
    if (created) *created = false;
    return -1;
}

int registry_find_or_create(registry_t *r, const device_id_t *id, uint32_t now_s)
{
    bool created;
    int idx = find_or_create(r, id, &created);
    /* Only the CREATE path gets a fresh last_seen_s -- an already-known
     * device returned here (created == false) keeps whatever last_seen_s a
     * real reading last gave it; see this function's doc comment
     * (registry.h) for why finding it here must not itself count as a
     * sighting for a device that has one already. */
    if (idx >= 0 && created) r->devices[idx].last_seen_s = now_s;
    return idx;
}

int registry_set_cap(registry_t *r, const device_id_t *id, uint8_t cap_id,
                      int16_t raw, uint32_t now_s)
{
    if (cap_id >= CAPABILITY_COUNT) return -1;
    int idx = find_or_create(r, id, NULL);
    if (idx < 0) return -1;
    device_entry_t *d = &r->devices[idx];
    d->last_seen_s = now_s;
    d->caps[cap_id].raw = raw;
    d->caps[cap_id].updated_s = now_s;
    d->caps[cap_id].valid = (raw != CAP_VALUE_NONE);
    return idx;
}

static void set_attribution(device_entry_t *d, const uint8_t via_node[6], int8_t rssi, uint32_t now_s)
{
    if (via_node) {
        d->via_node_valid = true;
        memcpy(d->via_node, via_node, 6);
    } else {
        d->via_node_valid = false;
        memset(d->via_node, 0, 6);
    }
    d->best_rssi = rssi;
    d->attributed_s = now_s;
}

bool registry_attribute(registry_t *r, const device_id_t *id, uint32_t frame_cnt,
                        const uint8_t via_node[6], int8_t rssi, uint32_t now_s)
{
    bool created;
    int idx = find_or_create(r, id, &created);
    if (idx < 0) return false;
    device_entry_t *d = &r->devices[idx];
    d->last_seen_s = now_s;

    if (created) {
        /* Brand-new device: unconditionally attributed to whoever reported
         * it first, exactly like V1's "new sensor" branch. */
        d->last_frame_cnt = (uint8_t)frame_cnt;
        set_attribution(d, via_node, rssi, now_s);
        return true;
    }

    if (d->last_frame_cnt == (uint8_t)frame_cnt) {
        /* Duplicate frame: attribution can still hand off to a stronger
         * reporter -- or unconditionally to a direct reception, which
         * always outranks a relay -- without this being counted as new
         * data. Only a fresh frame_cnt (below) resets the contest. */
        if (via_node == NULL) {
            set_attribution(d, NULL, rssi, now_s);
            return true;
        }
        if (d->via_node_valid && rssi > d->best_rssi) {
            /* Only contest attribution against another node's rssi; if the
             * hub already attributed this frame to itself directly, that
             * stands regardless of what any node claims. */
            set_attribution(d, via_node, rssi, now_s);
            return true;
        }
        return false;   /* repeated advertisement, this reporter doesn't own it */
    }

    /* New frame: the reporter of it unconditionally takes over. */
    d->last_frame_cnt = (uint8_t)frame_cnt;
    set_attribution(d, via_node, rssi, now_s);
    return true;
}

void registry_clear_attribution(registry_t *r, const uint8_t node_mac[6])
{
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        device_entry_t *d = &r->devices[i];
        if (!d->in_use || !d->via_node_valid) continue;
        if (memcmp(d->via_node, node_mac, 6) != 0) continue;
        d->via_node_valid = false;
        memset(d->via_node, 0, 6);
        d->best_rssi = 0;
    }
}
