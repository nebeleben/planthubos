#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "plants_migrate.h"

static const uint8_t MAC_A[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t MAC_B[6] = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };

int main(void)
{
    /* ---- empty in -> 0 actions ---- */
    {
        plants_table_t t;
        plants_table_init(&t);
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, NULL, 0, out, 8);
        assert(n == 0);
        assert(t.next_id == 1);
    }

    /* ---- one mac, name+raw+hourly -> 1 action, table gains plant 1
     * named+assigned, all three flags set. Table and `in` are reused below
     * for the idempotent re-run case. ---- */
    plants_table_t t;
    plants_table_init(&t);
    migrate_input_t in[1];
    memset(in, 0, sizeof(in));
    memcpy(in[0].mac, MAC_A, 6);
    in[0].has_name = true;
    strcpy(in[0].name, "Ficus");
    in[0].has_raw = true;
    in[0].has_hourly = true;
    {
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, in, 1, out, 8);
        assert(n == 1);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(out[0].plant_id == 1);
        assert(out[0].rename_raw == true);
        assert(out[0].rename_hourly == true);
        assert(out[0].move_name == true);

        int idx = plants_table_find_id(&t, 1);
        assert(idx >= 0);
        assert(t.p[idx].mac_valid == true);
        assert(memcmp(t.p[idx].mac, MAC_A, 6) == 0);
        assert(strcmp(t.p[idx].name, "Ficus") == 0);
    }

    /* ---- re-run with the same inputs against the RESULTING table -> 0
     * actions (idempotent): MAC_A is already known. ---- */
    {
        migrate_action_t out[8];
        int n = plants_migrate_plan(&t, in, 1, out, 8);
        assert(n == 0);
        assert(t.next_id == 2);   /* unchanged: no new plant created */
    }

    /* ---- mac with raw only -> action without move_name ---- */
    {
        plants_table_t t2;
        plants_table_init(&t2);
        migrate_input_t in2[1];
        memset(in2, 0, sizeof(in2));
        memcpy(in2[0].mac, MAC_B, 6);
        in2[0].has_raw = true;

        migrate_action_t out[8];
        int n = plants_migrate_plan(&t2, in2, 1, out, 8);
        assert(n == 1);
        assert(out[0].plant_id == 1);
        assert(out[0].rename_raw == true);
        assert(out[0].rename_hourly == false);
        assert(out[0].move_name == false);
    }

    /* ---- named-but-fileless sensor: no ring files, only a name -> still
     * gets a plant, no renames (nothing to rename). ---- */
    {
        plants_table_t t3;
        plants_table_init(&t3);
        migrate_input_t in3[1];
        memset(in3, 0, sizeof(in3));
        memcpy(in3[0].mac, MAC_A, 6);
        in3[0].has_name = true;
        strcpy(in3[0].name, "Nameless History");

        migrate_action_t out[8];
        int n = plants_migrate_plan(&t3, in3, 1, out, 8);
        assert(n == 1);
        assert(out[0].plant_id == 1);
        assert(out[0].rename_raw == false);
        assert(out[0].rename_hourly == false);
        assert(out[0].move_name == true);
        int idx = plants_table_find_id(&t3, 1);
        assert(idx >= 0);
        assert(strcmp(t3.p[idx].name, "Nameless History") == 0);
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
            big[i].has_raw = true;
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

    /* ---- two macs -> distinct ids in input order ---- */
    {
        plants_table_t t5;
        plants_table_init(&t5);
        migrate_input_t two[2];
        memset(two, 0, sizeof(two));
        memcpy(two[0].mac, MAC_A, 6);
        two[0].has_raw = true;
        memcpy(two[1].mac, MAC_B, 6);
        two[1].has_raw = true;

        migrate_action_t out[8];
        int n = plants_migrate_plan(&t5, two, 2, out, 8);
        assert(n == 2);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(out[0].plant_id == 1);
        assert(memcmp(out[1].mac, MAC_B, 6) == 0);
        assert(out[1].plant_id == 2);
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
        two[0].has_raw = true;
        memcpy(two[1].mac, MAC_B, 6);
        two[1].has_raw = true;

        migrate_action_t out[1];
        int n = plants_migrate_plan(&t6, two, 2, out, 1);
        assert(n == 1);
        assert(memcmp(out[0].mac, MAC_A, 6) == 0);
        assert(plants_table_find_mac(&t6, MAC_B) == -1);
    }

    printf("test_plants_migrate: OK\n");
    return 0;
}
