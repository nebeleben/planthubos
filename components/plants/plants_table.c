#include "plants_table.h"
#include <string.h>

void plants_table_init(plants_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->next_id = 1;
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
    memset(&t->p[idx], 0, sizeof(t->p[idx]));
    t->p[idx].in_use = false;
    return true;
}
