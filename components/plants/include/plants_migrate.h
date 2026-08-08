#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "plants_table.h"

/* One-boot migration planner (M8 Task 4) -- PURE, no ESP includes, host
 * compilable exactly like plants_table.c/.h. Turns "what the firmware found
 * lying around from the pre-M8 sensor-keyed world" into "what plants to
 * create and what to rename", without touching a filesystem or NVS itself
 * -- the executor (plants.c, firmware side) discovers migrate_input_t[] by
 * scanning /storage for legacy MAC-named ring files and enumerating NVS
 * sensor-name keys, calls plants_migrate_plan() once, then performs the
 * renames/deletes the returned migrate_action_t[] describes. */

typedef struct {
    uint8_t mac[6];
    bool    has_name;
    char    name[PLANT_NAME_LEN + 1];
    bool    has_raw, has_hourly;     /* MAC-named ring files present */
} migrate_input_t;

typedef struct {
    uint8_t mac[6];
    uint8_t plant_id;                /* target plant (created by the plan) */
    bool    rename_raw, rename_hourly;
    bool    move_name;
} migrate_action_t;

/* Given the discovered inputs and the CURRENT table (normally empty; may
 * already contain plants on a re-run), produce actions and mutate the table
 * (creates + names + assignments). Skips macs already known to the table
 * (idempotency: the executor's second boot finds no MAC-named files and no
 * orphan NVS names, so `in` is naturally empty then too -- this skip is the
 * belt to that braces, e.g. for a plant created by other means in between).
 * Excess input beyond the table's remaining capacity (or beyond `max_out`)
 * produces no action and no table growth for those entries; earlier entries
 * still succeed. Returns the number of actions written to `out`. */
int plants_migrate_plan(plants_table_t *t,
                        const migrate_input_t *in, int n_in,
                        migrate_action_t *out, int max_out);
