#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "plants_migrate.h"
#include "plants_blob.h"

/* M2 note (task-4-report.md): this file used to also assert
 * rename_raw/rename_hourly propagation from migrate_input_t.has_raw/
 * has_hourly (V1 MAC-named ring-file adoption) -- dropped here. Task 5's
 * clean start deletes any V1 ring files outright rather than migrating
 * them, so migrate_scan_ring_files() (plants.c, untouched by this task)
 * will never again find one to feed into plants_migrate_plan(); that path
 * is dead input from here on even though plants_migrate.c's own field
 * propagation is untouched code. What remains meaningful -- and is still
 * covered below -- is plants_migrate_plan()'s id-retirement/allocation
 * behavior: ids assigned in input order, an already-known mac produces no
 * new id (idempotent re-run), and both the table's PLANTS_MAX capacity and
 * the caller's max_out buffer bound the count of ids handed out. NVS
 * sensor-name adoption (has_name/move_name) is unrelated to ring files and
 * stays covered too. */

static const uint8_t MAC_A[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t MAC_B[6] = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };

/* ---- plants.bin blob format-2 -> format-3 migration (M5b Task 2) ----
 *
 * This is a DIFFERENT "migration" than plants_migrate_plan() above: that
 * one turns pre-M8 sensor-keyed filesystem/NVS state into plants; this one
 * turns an OLDER plants.bin BLOB BYTE LAYOUT (plants_blob.h's
 * plant_entry_blob_v2_t, from before CAPABILITY_COUNT grew 8->9 and action
 * slots were added) into the current plants_table_t shape, exactly the way
 * plants.c's load_file() will on a real upgrading hub. plants_blob_load()
 * is the same pure function plants.c calls -- this proves the real
 * migration code, not a reimplementation of it. */

static device_id_t v2_dev(const uint8_t mac[6])
{
    return device_id_from_mac(DEV_KIND_BLE, mac);
}

/* Builds one format-2 blob, by hand, matching plant_entry_blob_v2_t's byte
 * layout field by field (mirrors how plants.c's pack_blob() used to build
 * this exact shape before Task 2 bumped the format) -- two plants, each
 * with a couple of capability bindings, so the test can show BOTH survive
 * and stay attached to the right plant. */
static void build_v2_blob(plants_blob_v2_t *b)
{
    memset(b, 0, sizeof(*b));
    b->format = PLANTS_BLOB_FORMAT_V2;
    b->next_id = 3;

    b->p[0].id = 1;
    b->p[0].in_use = 1;
    memcpy(b->p[0].mac, MAC_A, 6);
    b->p[0].mac_valid = 1;
    strcpy(b->p[0].name, "Ficus");
    device_id_t dev_a = v2_dev(MAC_A);
    b->p[0].cap_bound[CAP_SOIL_MOISTURE] = 1;
    b->p[0].cap_dev[CAP_SOIL_MOISTURE] = dev_a;
    b->p[0].cap_bound[CAP_AIR_TEMPERATURE] = 1;
    b->p[0].cap_dev[CAP_AIR_TEMPERATURE] = dev_a;

    b->p[1].id = 2;
    b->p[1].in_use = 1;
    memcpy(b->p[1].mac, MAC_B, 6);
    b->p[1].mac_valid = 1;
    strcpy(b->p[1].name, "Monstera");
    device_id_t dev_b = v2_dev(MAC_B);
    b->p[1].cap_bound[CAP_SOIL_CONDUCTIVITY] = 1;
    b->p[1].cap_dev[CAP_SOIL_CONDUCTIVITY] = dev_b;
}

static void test_blob_v2_migration(void)
{
    plants_blob_v2_t v2;
    build_v2_blob(&v2);

    plants_table_t t;
    plants_table_init(&t);
    assert(plants_blob_load((const uint8_t *)&v2, sizeof(v2), &t) == true);

    /* next_id and both plants' identity/name/mac survive. */
    assert(t.next_id == 3);
    int idx0 = plants_table_find_id(&t, 1);
    int idx1 = plants_table_find_id(&t, 2);
    assert(idx0 >= 0 && idx1 >= 0);
    assert(strcmp(t.p[idx0].name, "Ficus") == 0);
    assert(t.p[idx0].mac_valid && memcmp(t.p[idx0].mac, MAC_A, 6) == 0);
    assert(strcmp(t.p[idx1].name, "Monstera") == 0);
    assert(t.p[idx1].mac_valid && memcmp(t.p[idx1].mac, MAC_B, 6) == 0);

    /* the pre-existing capability bindings survive, attached to the right
     * plant, with the right device. */
    device_id_t dev_a = v2_dev(MAC_A), dev_b = v2_dev(MAC_B);
    assert(t.p[idx0].cap_bound[CAP_SOIL_MOISTURE]);
    assert(device_id_equal(&t.p[idx0].cap_dev[CAP_SOIL_MOISTURE], &dev_a));
    assert(t.p[idx0].cap_bound[CAP_AIR_TEMPERATURE]);
    assert(device_id_equal(&t.p[idx0].cap_dev[CAP_AIR_TEMPERATURE], &dev_a));
    assert(t.p[idx1].cap_bound[CAP_SOIL_CONDUCTIVITY]);
    assert(device_id_equal(&t.p[idx1].cap_dev[CAP_SOIL_CONDUCTIVITY], &dev_b));

    /* the NEW capability (switch.state, id 8) comes back unbound on every
     * migrated plant -- a format-2 blob never had it. */
    assert(!t.p[idx0].cap_bound[CAP_SWITCH_STATE]);
    assert(!t.p[idx1].cap_bound[CAP_SWITCH_STATE]);

    /* both action slots come back unbound/ACTION_NONE on every migrated
     * plant -- a format-2 blob never had action slots at all. */
    for (uint8_t s = 0; s < PLANT_ACTION_SLOTS; s++) {
        assert(!t.p[idx0].act_bound[s]);
        assert(t.p[idx0].act_id[s] == ACTION_NONE);
        assert(!t.p[idx1].act_bound[s]);
        assert(t.p[idx1].act_id[s] == ACTION_NONE);
    }

    /* a length that matches neither the current nor the v2 blob size is
     * rejected outright (the "unrecognised size" discard load_file() has
     * always had, unchanged by this migration). */
    plants_table_t t2;
    plants_table_init(&t2);
    uint8_t truncated[sizeof(plants_blob_v2_t) - 1];
    memcpy(truncated, &v2, sizeof(truncated));
    assert(plants_blob_load(truncated, sizeof(truncated), &t2) == false);

    /* a v2-sized blob with the WRONG format byte is also rejected, not
     * misread as v2. */
    plants_blob_v2_t bad_fmt = v2;
    bad_fmt.format = 99;
    plants_table_t t3;
    plants_table_init(&t3);
    assert(plants_blob_load((const uint8_t *)&bad_fmt, sizeof(bad_fmt), &t3) == false);
}

int main(void)
{
    test_blob_v2_migration();

    /* ---- empty in -> 0 actions ---- */
    {
        plants_table_t t;
        plants_table_init(&t);
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, NULL, 0, out, 8);
        assert(n == 0);
        assert(t.next_id == 1);
    }

    /* ---- one named mac -> 1 action, table gains a named plant with that
     * mac. Table and `in` are reused below for the idempotent re-run
     * case. ---- */
    plants_table_t t;
    plants_table_init(&t);
    migrate_input_t in[1];
    memset(in, 0, sizeof(in));
    memcpy(in[0].mac, MAC_A, 6);
    in[0].has_name = true;
    strcpy(in[0].name, "Ficus");
    {
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, in, 1, out, 8);
        assert(n == 1);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(out[0].plant_id == 1);
        assert(out[0].move_name == true);

        int idx = plants_table_find_id(&t, 1);
        assert(idx >= 0);
        assert(t.p[idx].mac_valid == true);
        assert(memcmp(t.p[idx].mac, MAC_A, 6) == 0);
        assert(strcmp(t.p[idx].name, "Ficus") == 0);
    }

    /* ---- re-run with the same inputs against the RESULTING table -> 0
     * actions (id retirement: MAC_A is already known, no new id is
     * allocated). ---- */
    {
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, in, 1, out, 8);
        assert(n == 0);
        assert(t.next_id == 2);   /* unchanged: no new plant created */
    }

    /* ---- two macs -> distinct ids in input order ---- */
    {
        plants_table_t t5;
        plants_table_init(&t5);
        migrate_input_t two[2];
        memset(two, 0, sizeof(two));
        memcpy(two[0].mac, MAC_A, 6);
        memcpy(two[1].mac, MAC_B, 6);

        migrate_action_t out[8];
        int n = plants_migrate_plan(&t5, two, 2, out, 8);
        assert(n == 2);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(out[0].plant_id == 1);
        assert(memcmp(out[1].mac, MAC_B, 6) == 0);
        assert(out[1].plant_id == 2);
    }

    /* ---- more inputs than PLANTS_MAX -> excess produce no actions (and no
     * table growth), count reflects only executed ones ---- */
    {
        plants_table_t t4;
        plants_table_init(&t4);
        migrate_input_t big[PLANTS_MAX + 4];
        memset(big, 0, sizeof(big));
        for (int i = 0; i < PLANTS_MAX + 4; i++) {
            big[i].mac[0] = 0xAA;
            big[i].mac[1] = 0xBB;
            big[i].mac[2] = 0xCC;
            big[i].mac[3] = 0x00;
            big[i].mac[4] = 0x00;
            big[i].mac[5] = (uint8_t)i;
        }
        migrate_action_t out[PLANTS_MAX + 4];
        int n = plants_migrate_plan(&t4, big, PLANTS_MAX + 4, out, PLANTS_MAX + 4);
        assert(n == PLANTS_MAX);

        int used = 0;
        for (int i = 0; i < PLANTS_MAX; i++) {
            if (t4.p[i].in_use) used++;
        }
        assert(used == PLANTS_MAX);
        assert(t4.next_id == (uint8_t)(PLANTS_MAX + 1));   /* no growth past cap */
    }

    /* ---- max_out smaller than the number of eligible macs: no overflow,
     * count is bounded by max_out, and the un-recorded macs are NOT
     * consumed from the table (so a caller could retry with a bigger
     * buffer). ---- */
    {
        plants_table_t t6;
        plants_table_init(&t6);
        migrate_input_t two[2];
        memset(two, 0, sizeof(two));
        memcpy(two[0].mac, MAC_A, 6);
        memcpy(two[1].mac, MAC_B, 6);

        migrate_action_t out[1];
        int n = plants_migrate_plan(&t6, two, 2, out, 1);
        assert(n == 1);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(plants_table_find_mac(&t6, MAC_B) == -1);
    }

    printf("test_plants_migrate: OK\n");
    return 0;
}
