#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "plants_table.h"

static const uint8_t MAC_A[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t MAC_B[6] = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
static const uint8_t MAC_C[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static device_id_t dev_ble(const uint8_t mac[6])
{
    return device_id_from_mac(DEV_KIND_BLE, mac);
}

/* plants_table_create() takes an optional probe mac, not a name (unlike the
 * brief's illustrative snippet) -- name it afterwards via
 * plants_table_rename(), same two-step every other test in this file uses. */
static uint8_t make_named_plant(plants_table_t *t, const char *name)
{
    uint8_t pid = plants_table_create(t, NULL);
    assert(pid != 0);
    assert(plants_table_rename(t, pid, name));
    return pid;
}

static void test_bind_action(void)
{
    plants_table_t t; plants_table_init(&t);
    uint8_t pid = make_named_plant(&t, "Ficus");
    device_id_t dev = device_id_from_mac(DEV_KIND_BLE, (uint8_t[]){1,2,3,4,5,6});

    assert(plants_table_bind_action(&t, pid, 0, ACT_IRRIGATION_OPEN, &dev));
    assert(plants_table_action_slot(&t, pid, ACT_IRRIGATION_OPEN) == 0);
    assert(plants_table_action_slot(&t, pid, ACT_PUMP_RUN) == -1);

    /* Slot index is bounded by PLANT_ACTION_SLOTS (spec section 8: two slots
     * per plant, because the hub supports four actor devices in total). */
    assert(!plants_table_bind_action(&t, pid, PLANT_ACTION_SLOTS, ACT_SWITCH_ON, &dev));
    assert(!plants_table_bind_action(&t, pid, 0, ACTION_COUNT, &dev));
}

/* Swapping the device keeps the plant's identity, exactly as capability
 * bindings already do. */
static void test_rebind_action_keeps_plant(void)
{
    plants_table_t t; plants_table_init(&t);
    uint8_t pid = make_named_plant(&t, "Ficus");
    device_id_t a = device_id_from_mac(DEV_KIND_BLE, (uint8_t[]){1,1,1,1,1,1});
    device_id_t b = device_id_from_mac(DEV_KIND_BLE, (uint8_t[]){2,2,2,2,2,2});
    assert(plants_table_bind_action(&t, pid, 0, ACT_IRRIGATION_OPEN, &a));
    assert(plants_table_bind_action(&t, pid, 0, ACT_IRRIGATION_OPEN, &b));
    assert(device_id_equal(&t.p[0].act_dev[0], &b));
    assert(t.p[0].id == pid);
}

int main(void)
{
    plants_table_t t;
    plants_table_init(&t);
    assert(t.next_id == 1);

    /* ---- create/rename/delete: unchanged from V1 ---- */

    uint8_t id_a = plants_table_resolve(&t, MAC_A);
    assert(id_a == 1);
    assert(t.next_id == 2);
    int idx = plants_table_find_id(&t, 1);
    assert(idx >= 0);
    assert(t.p[idx].in_use);
    assert(t.p[idx].mac_valid);
    assert(memcmp(t.p[idx].mac, MAC_A, 6) == 0);

    /* resolve(macA) again returns the same id, no dup created */
    uint8_t id_a2 = plants_table_resolve(&t, MAC_A);
    assert(id_a2 == 1);
    assert(t.next_id == 2);

    /* create(NULL) -> empty plant */
    uint8_t id_empty = plants_table_create(&t, NULL);
    assert(id_empty == 2);
    assert(t.next_id == 3);
    idx = plants_table_find_id(&t, 2);
    assert(idx >= 0);
    assert(t.p[idx].mac_valid == false);
    assert(strcmp(t.p[idx].name, "") == 0);

    /* rename bounds: 33-char name rejected, 32 accepted */
    char name32[PLANT_NAME_LEN + 1];
    memset(name32, 'x', PLANT_NAME_LEN);
    name32[PLANT_NAME_LEN] = '\0';
    assert(strlen(name32) == 32);

    char name33[PLANT_NAME_LEN + 2];
    memset(name33, 'x', PLANT_NAME_LEN + 1);
    name33[PLANT_NAME_LEN + 1] = '\0';
    assert(strlen(name33) == 33);

    assert(plants_table_rename(&t, 2, name33) == false);
    assert(plants_table_rename(&t, 2, name32) == true);
    idx = plants_table_find_id(&t, 2);
    assert(strcmp(t.p[idx].name, name32) == 0);

    /* rename/delete on unknown id fail */
    assert(plants_table_rename(&t, 99, "x") == false);
    assert(plants_table_delete(&t, 99) == false);

    uint8_t id_c = plants_table_resolve(&t, MAC_C);
    assert(id_c == 3);
    assert(t.next_id == 4);

    /* plants_table_assign() has no production caller left as of Task 7
     * (plants.h's plants_assign() wrapper -- the last one -- was deleted
     * along with the rest of the M2 registry/storage compatibility shims,
     * RULING-1), but the primitive itself stays: minimal coverage here for
     * its V1 "assigning a mac already assigned elsewhere MOVES it"
     * semantics, and NULL unassigns. */
    assert(plants_table_assign(&t, id_empty, MAC_C) == true);   /* moves MAC_C off id_c onto id_empty */
    assert(plants_table_find_mac(&t, MAC_C) == plants_table_find_id(&t, id_empty));
    assert(t.p[plants_table_find_id(&t, id_c)].mac_valid == false);
    assert(plants_table_assign(&t, id_empty, NULL) == true);
    assert(t.p[plants_table_find_id(&t, id_empty)].mac_valid == false);

    /* delete(2) retires: find_id(2) == -1, id 2 never reused */
    assert(plants_table_delete(&t, id_empty) == true);
    assert(plants_table_find_id(&t, id_empty) == -1);
    assert(t.next_id == 4);   /* delete never touches next_id */

    /* currently in_use: ids 1, 3 (id 2 retired) = 2 entries. Fill remaining
     * PLANTS_MAX - 2 slots to hit capacity -- create/delete's fill/full/
     * fresh-id-after-delete behavior, unchanged from V1. */
    uint8_t last_id = id_c;
    for (int i = 0; i < PLANTS_MAX - 2; i++) {
        uint8_t new_id = plants_table_create(&t, NULL);
        assert(new_id != 0);
        assert(new_id == (uint8_t)(last_id + 1));
        assert(t.next_id == (uint8_t)(new_id + 1));
        last_id = new_id;
    }

    /* table now full: create returns 0, next_id unchanged */
    assert(plants_table_create(&t, NULL) == 0);
    assert(t.next_id == (uint8_t)(last_id + 1));

    /* delete one -> create succeeds with a FRESH id (never reused) */
    uint8_t reused_slot_id = (uint8_t)(id_c + 1);
    assert(plants_table_delete(&t, reused_slot_id) == true);
    assert(plants_table_find_id(&t, reused_slot_id) == -1);
    uint8_t fresh_id = plants_table_create(&t, NULL);
    assert(fresh_id != 0);
    assert(fresh_id != reused_slot_id);
    assert(fresh_id == (uint8_t)(last_id + 1));
    assert(t.next_id == (uint8_t)(fresh_id + 1));
    /* free that slot back up again for the bindings section below */
    assert(plants_table_delete(&t, fresh_id) == true);

    /* ---- capability bindings (M2) ---- */

    device_id_t dev_a = dev_ble(MAC_A);
    device_id_t dev_b = dev_ble(MAC_B);

    /* bind one capability then read it back */
    assert(plants_table_bind_cap(&t, id_a, CAP_SOIL_MOISTURE, &dev_a) == true);
    plant_binding_t out[CAPABILITY_COUNT];
    size_t n = plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT);
    assert(n == 1);
    assert(out[0].cap_id == CAP_SOIL_MOISTURE);
    assert(device_id_equal(&out[0].dev, &dev_a));

    /* binding a second capability to a DIFFERENT device keeps both */
    assert(plants_table_bind_cap(&t, id_a, CAP_AIR_TEMPERATURE, &dev_b) == true);
    n = plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT);
    assert(n == 2);
    bool saw_moisture = false, saw_temp = false;
    for (size_t i = 0; i < n; i++) {
        if (out[i].cap_id == CAP_SOIL_MOISTURE) {
            saw_moisture = true;
            assert(device_id_equal(&out[i].dev, &dev_a));
        } else if (out[i].cap_id == CAP_AIR_TEMPERATURE) {
            saw_temp = true;
            assert(device_id_equal(&out[i].dev, &dev_b));
        }
    }
    assert(saw_moisture && saw_temp);

    /* plants_table_bind_cap(..., NULL) clears just that one binding */
    assert(plants_table_bind_cap(&t, id_a, CAP_SOIL_MOISTURE, NULL) == true);
    n = plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT);
    assert(n == 1);
    assert(out[0].cap_id == CAP_AIR_TEMPERATURE);
    assert(device_id_equal(&out[0].dev, &dev_b));

    /* two plants may bind the same device (V2 lifts V1's one-probe-per-plant
     * restriction -- see plants_table_bind_cap()'s doc comment) */
    assert(plants_table_bind_cap(&t, id_c, CAP_AIR_TEMPERATURE, &dev_b) == true);
    n = plants_table_bindings(&t, id_c, out, CAPABILITY_COUNT);
    assert(n == 1);
    assert(out[0].cap_id == CAP_AIR_TEMPERATURE);
    assert(device_id_equal(&out[0].dev, &dev_b));
    /* plant id_a's own binding to dev_b is untouched by id_c also binding it */
    n = plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT);
    assert(n == 1);
    assert(out[0].cap_id == CAP_AIR_TEMPERATURE);

    /* binding an unknown capability id is rejected */
    assert(plants_table_bind_cap(&t, id_a, CAPABILITY_COUNT, &dev_a) == false);
    assert(plants_table_bind_cap(&t, id_a, 200, &dev_a) == false);
    n = plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT);
    assert(n == 1);   /* unchanged by the rejected calls */

    /* binding on an unknown plant id is rejected */
    assert(plants_table_bind_cap(&t, 99, CAP_SOIL_MOISTURE, &dev_a) == false);
    assert(plants_table_bindings(&t, 99, out, CAPABILITY_COUNT) == 0);

    /* deleting a plant drops its bindings */
    assert(plants_table_delete(&t, id_a) == true);
    assert(plants_table_find_id(&t, id_a) == -1);
    /* id never reused, so plants_table_bindings() on the retired id reports
     * "unknown plant" (0), not stale bindings */
    assert(plants_table_bindings(&t, id_a, out, CAPABILITY_COUNT) == 0);
    /* id_c's binding survives id_a's deletion */
    n = plants_table_bindings(&t, id_c, out, CAPABILITY_COUNT);
    assert(n == 1);
    assert(out[0].cap_id == CAP_AIR_TEMPERATURE);

    /* a freshly created plant starts with no bindings (memset in
     * plants_table_create() covers the new fields too, same slot id_a just
     * vacated) */
    uint8_t id_d = plants_table_create(&t, NULL);
    assert(id_d != 0 && id_d != id_a);
    assert(plants_table_bindings(&t, id_d, out, CAPABILITY_COUNT) == 0);

    test_bind_action();
    test_rebind_action_keeps_plant();

    printf("test_plants_table: OK\n");
    return 0;
}
