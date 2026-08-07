#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PLANTS_MAX      16
#define PLANT_NAME_LEN  32

typedef struct {
    bool    in_use;
    uint8_t id;                       /* 1-based, never reused */
    char    name[PLANT_NAME_LEN + 1]; /* "" = unnamed */
    bool    mac_valid;
    uint8_t mac[6];
} plant_entry_t;

typedef struct {
    uint8_t       next_id;            /* first unallocated id; starts at 1 */
    plant_entry_t p[PLANTS_MAX];
} plants_table_t;

void plants_table_init(plants_table_t *t);

/* Create a plant (optionally with a probe mac). Returns the new id, or 0
 * when the table is full OR next_id would wrap past 255. */
uint8_t plants_table_create(plants_table_t *t, const uint8_t *mac_or_null);

/* Find helpers: return the entry index or -1. */
int plants_table_find_id(const plants_table_t *t, uint8_t id);
int plants_table_find_mac(const plants_table_t *t, const uint8_t mac[6]);

/* Resolve mac -> plant id, auto-creating when unknown. 0 = full. */
uint8_t plants_table_resolve(plants_table_t *t, const uint8_t mac[6]);

/* Assignment moves per spec §2: assigning a mac already assigned elsewhere
 * moves it (old plant loses its probe); assigning to a plant that has one
 * replaces it (old mac becomes unassigned everywhere). mac == NULL
 * unassigns. Returns false for unknown id. */
bool plants_table_assign(plants_table_t *t, uint8_t id, const uint8_t *mac_or_null);

bool plants_table_rename(plants_table_t *t, uint8_t id, const char *name);
bool plants_table_delete(plants_table_t *t, uint8_t id);   /* id stays retired */
