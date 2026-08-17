#include "plants_table.h"
#include <string.h>

void plants_table_init(plants_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->next_id = 1;
    /* act_bound[]==false already makes every slot read as unbound, but
     * act_id[]'s memset-0 would otherwise read as ACT_SWITCH_ON (a real
     * action id, not "none") -- pin it explicitly, same as
     * plants_table_create()/plants_table_delete() below. */
    for (int i = 0; i < PLANTS_MAX; i++) {
        for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
            t->p[i].act_id[s] = ACTION_NONE;
        }
    }
}

static int find_free_slot(const plants_table_t *t)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (!t->p[i].in_use) {
            return i;
        }
    }
    return -1;
}

uint8_t plants_table_create(plants_table_t *t, const uint8_t *mac_or_null)
{
    if (t->next_id == 0) {
        return 0; /* wrapped past 255: no more ids available */
    }

    int slot = find_free_slot(t);
    if (slot < 0) {
        return 0; /* table full */
    }

    plant_entry_t *e = &t->p[slot];
    memset(e, 0, sizeof(*e));
    e->in_use = true;
    e->id = t->next_id;
    e->name[0] = '\0';
    for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
        e->act_id[s] = ACTION_NONE;   /* see plants_table_init()'s comment */
    }
    if (mac_or_null != NULL) {
        e->mac_valid = true;
        memcpy(e->mac, mac_or_null, 6);
    } else {
        e->mac_valid = false;
    }

    uint8_t new_id = t->next_id;
    t->next_id++; /* may wrap to 0, handled on next create */
    return new_id;
}

int plants_table_find_id(const plants_table_t *t, uint8_t id)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (t->p[i].in_use && t->p[i].id == id) {
            return i;
        }
    }
    return -1;
}

int plants_table_find_mac(const plants_table_t *t, const uint8_t mac[6])
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (t->p[i].in_use && t->p[i].mac_valid && memcmp(t->p[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

uint8_t plants_table_resolve(plants_table_t *t, const uint8_t mac[6])
{
    int idx = plants_table_find_mac(t, mac);
    if (idx >= 0) {
        return t->p[idx].id;
    }
    return plants_table_create(t, mac);
}

bool plants_table_assign(plants_table_t *t, uint8_t id, const uint8_t *mac_or_null)
{
    int idx = plants_table_find_id(t, id);
    if (idx < 0) {
        return false;
    }

    if (mac_or_null == NULL) {
        t->p[idx].mac_valid = false;
        return true;
    }

    /* If this mac is currently assigned elsewhere, move it: the old plant
     * loses its probe. */
    int other = plants_table_find_mac(t, mac_or_null);
    if (other >= 0 && other != idx) {
        t->p[other].mac_valid = false;
    }

    /* Replaces whatever mac (if any) this plant already had. */
    t->p[idx].mac_valid = true;
    memcpy(t->p[idx].mac, mac_or_null, 6);
    return true;
}

bool plants_table_rename(plants_table_t *t, uint8_t id, const char *name)
{
    int idx = plants_table_find_id(t, id);
    if (idx < 0) {
        return false;
    }
    if (strlen(name) > PLANT_NAME_LEN) {
        return false;
    }
    strcpy(t->p[idx].name, name);
    return true;
}

bool plants_table_delete(plants_table_t *t, uint8_t id)
{
    int idx = plants_table_find_id(t, id);
    if (idx < 0) {
        return false;
    }
    memset(&t->p[idx], 0, sizeof(t->p[idx]));   /* also drops every binding */
    t->p[idx].in_use = false;
    for (int s = 0; s < PLANT_ACTION_SLOTS; s++) {
        t->p[idx].act_id[s] = ACTION_NONE;   /* see plants_table_init()'s comment */
    }
    return true;
}

bool plants_table_bind_cap(plants_table_t *t, uint8_t id, uint8_t cap_id,
                           const device_id_t *dev)
{
    if (cap_id >= CAPABILITY_COUNT) {
        return false;
    }
    int idx = plants_table_find_id(t, id);
    if (idx < 0) {
        return false;
    }
    if (dev == NULL) {
        t->p[idx].cap_bound[cap_id] = false;
        memset(&t->p[idx].cap_dev[cap_id], 0, sizeof(device_id_t));
        return true;
    }
    t->p[idx].cap_bound[cap_id] = true;
    t->p[idx].cap_dev[cap_id] = *dev;
    return true;
}

size_t plants_table_bindings(const plants_table_t *t, uint8_t id,
                             plant_binding_t *out, size_t max)
{
    int idx = plants_table_find_id(t, id);
    if (idx < 0) {
        return 0;
    }
    size_t n = 0;
    for (uint8_t cap = 0; cap < CAPABILITY_COUNT && n < max; cap++) {
        if (t->p[idx].cap_bound[cap]) {
            out[n].cap_id = cap;
            out[n].dev = t->p[idx].cap_dev[cap];
            n++;
        }
    }
    return n;
}

bool plants_table_bind_action(plants_table_t *t, uint8_t plant_id, uint8_t slot,
                              uint8_t action_id, const device_id_t *dev)
{
    if (!dev || slot >= PLANT_ACTION_SLOTS || action_id >= ACTION_COUNT) {
        return false;
    }
    int idx = plants_table_find_id(t, plant_id);
    if (idx < 0) {
        return false;
    }
    t->p[idx].act_bound[slot] = true;
    t->p[idx].act_id[slot] = action_id;
    t->p[idx].act_dev[slot] = *dev;
    return true;
}

int plants_table_action_slot(const plants_table_t *t, uint8_t plant_id, uint8_t action_id)
{
    int idx = plants_table_find_id(t, plant_id);
    if (idx < 0) {
        return -1;
    }
    for (uint8_t s = 0; s < PLANT_ACTION_SLOTS; s++) {
        if (t->p[idx].act_bound[s] && t->p[idx].act_id[s] == action_id) {
            return s;
        }
    }
    return -1;
}
