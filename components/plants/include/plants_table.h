#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capability.h"   /* CAPABILITY_COUNT, device_id_t */
#include "action.h"       /* ACTION_COUNT, ACTION_NONE (M5b action slots) */

#define PLANTS_MAX      16
#define PLANT_NAME_LEN  32

/* Two, not four (spec section 8). The budget did not close at four: 16
 * plants x 4 slots x 10 B is 640 B against 1280 B of total headroom, and
 * the hub supports four actor devices in TOTAL. A plant binding more than
 * two actuators is not a V2 shape. */
#define PLANT_ACTION_SLOTS 2

/* M2: per-capability device bindings, indexed by cap_id (0..CAPABILITY_COUNT-1)
 * -- cap_bound[c] tells whether capability c is bound at all, cap_dev[c] is
 * which physical device it's bound to when it is. A plant may bind
 * different capabilities to different devices (e.g. soil moisture from one
 * probe, air temperature from another), and the same device may be bound
 * by more than one plant (V2 lifts V1's "one probe, one plant" restriction
 * -- see plants_table_bind_cap()). mac/mac_valid below are the OLDER V1
 * single-probe-per-plant fields, still live internally for plants.c's
 * mac-keyed auto-create/resolve flow (plants_table_resolve()/
 * plants_table_create()/the sensor-keyed migration, plants_migrate.c) --
 * bindings are additive, not a replacement for them; see task-4-report.md.
 * As of Task 7 (task-7-report.md), every EXTERNAL consumer that used to
 * read mac/mac_valid directly for display/publish purposes
 * (sensors_json.c, now deleted; influx.c, mqtt_pub.c) reads capability
 * bindings instead (plants_bindings()/plants_cap_value()) -- these two
 * fields are internal plumbing now, not a second public identity. */
typedef struct {
    bool    in_use;
    uint8_t id;                       /* 1-based, never reused */
    char    name[PLANT_NAME_LEN + 1]; /* "" = unnamed */
    bool    mac_valid;
    uint8_t mac[6];
    bool        cap_bound[CAPABILITY_COUNT];
    device_id_t cap_dev[CAPABILITY_COUNT];
    /* M5b: per-plant action slots, the actuator analogue of cap_bound[]/
     * cap_dev[] above -- act_bound[s] tells whether slot s is bound at all,
     * act_id[s] is which action.h action id (ACTION_NONE when unbound),
     * act_dev[s] is which physical device it's bound to. See
     * PLANT_ACTION_SLOTS above for why there are only two. */
    bool        act_bound[PLANT_ACTION_SLOTS];
    uint8_t     act_id[PLANT_ACTION_SLOTS];      /* ACTION_NONE when unbound */
    device_id_t act_dev[PLANT_ACTION_SLOTS];
} plant_entry_t;

/* Produced (out-projection) shape for plants_table_bindings()/plants_bindings():
 * one entry per currently-bound capability, cap_id identifying which one. */
typedef struct { uint8_t cap_id; device_id_t dev; } plant_binding_t;

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

/* Bind one capability on plant `id` to `dev` (dev == NULL clears just that
 * one capability, leaving every other binding on the plant untouched).
 * Rebinding an already-bound capability replaces its device outright --
 * unlike plants_table_assign()'s V1 mac, a capability binding has no "move
 * it off whoever else has it" side effect: two plants may bind the same
 * device (V1's one-plant-per-probe restriction does not apply here).
 * Returns false for an unknown plant id or cap_id >= CAPABILITY_COUNT. */
bool plants_table_bind_cap(plants_table_t *t, uint8_t id, uint8_t cap_id,
                           const device_id_t *dev);

/* Copies plant `id`'s currently-bound capabilities into out[] in cap_id
 * order (max CAPABILITY_COUNT entries). Returns the count actually copied
 * -- 0 for an unknown id or a plant with no bindings. */
size_t plants_table_bindings(const plants_table_t *t, uint8_t id,
                             plant_binding_t *out, size_t max);

/* Bind plant `plant_id`'s action slot `slot` to `action_id` on `dev`.
 * Rebinding an already-bound slot replaces its action/device outright, same
 * "no move-off side effect" contract as plants_table_bind_cap(). Returns
 * false for an unknown plant id, slot >= PLANT_ACTION_SLOTS, action_id >=
 * ACTION_COUNT, or dev == NULL (unlike plants_table_bind_cap(), a slot has
 * no "clear" form here -- rebind it, or leave it as ACTION_NONE). */
bool plants_table_bind_action(plants_table_t *t, uint8_t plant_id, uint8_t slot,
                              uint8_t action_id, const device_id_t *dev);

/* Which slot (0..PLANT_ACTION_SLOTS-1) plant `plant_id` has bound to
 * `action_id`, or -1 if none does (including an unknown plant id). */
int plants_table_action_slot(const plants_table_t *t, uint8_t plant_id, uint8_t action_id);
