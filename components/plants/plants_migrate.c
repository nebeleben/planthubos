#include "plants_migrate.h"
#include <string.h>

/* Pure planner (see plants_migrate.h): decide which macs from `in` need a
 * new plant and what to do with each one's files/name, WITHOUT touching a
 * filesystem or NVS -- the executor (plants.c) does the actual rename()s
 * and NVS-name deletion per the actions this returns.
 *
 * One migrate_action_t per distinct mac in `in`, in input order (mirrors
 * plants_table_create()'s in-order, monotonic id allocation -- see
 * plants_table.c). A mac already known to `t` (plants_table_find_mac()
 * hits) is idempotency's belt: skipped, no action, table untouched for it.
 * A mac that plants_table_create() cannot place -- table full, or `out` is
 * already at `max_out` -- also produces no action and the table is left
 * exactly as it was for that mac (so a caller could legitimately retry
 * later with room freed up or a bigger `out` buffer). */
int plants_migrate_plan(plants_table_t *t,
                        const migrate_input_t *in, int n_in,
                        migrate_action_t *out, int max_out)
{
    if (!t || !out) return 0;

    int n_out = 0;
    for (int i = 0; i < n_in; i++) {
        const migrate_input_t *mi = &in[i];

        if (plants_table_find_mac(t, mi->mac) >= 0) continue;   /* already migrated */
        if (n_out >= max_out) continue;                          /* out buffer full */

        uint8_t id = plants_table_create(t, mi->mac);
        if (id == 0) continue;   /* table full: no plant created, no action */

        if (mi->has_name) {
            plants_table_rename(t, id, mi->name);
        }

        migrate_action_t *a = &out[n_out];
        memcpy(a->mac, mi->mac, 6);
        a->plant_id = id;
        a->rename_raw = mi->has_raw;
        a->rename_hourly = mi->has_hourly;
        a->move_name = mi->has_name;
        n_out++;
    }
    return n_out;
}
