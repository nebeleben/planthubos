#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "plants_table.h"

static const uint8_t MAC_A[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t MAC_B[6] = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
static const uint8_t MAC_C[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

int main(void)
{
    plants_table_t t;
    plants_table_init(&t);
    assert(t.next_id == 1);

    /* resolve(macA) creates plant 1 */
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

    /* resolve(macB) -> 2 */
    uint8_t id_b = plants_table_resolve(&t, MAC_B);
    assert(id_b == 2);
    assert(t.next_id == 3);

    /* create(NULL) -> 3, empty plant */
    uint8_t id_empty = plants_table_create(&t, NULL);
    assert(id_empty == 3);
    assert(t.next_id == 4);
    idx = plants_table_find_id(&t, 3);
    assert(idx >= 0);
    assert(t.p[idx].mac_valid == false);
    assert(strcmp(t.p[idx].name, "") == 0);

    /* assign(3, macB) moves macB off plant 2 onto plant 3 */
    assert(plants_table_assign(&t, 3, MAC_B) == true);
    assert(t.next_id == 4); /* assign never allocates an id */
    int idx2 = plants_table_find_id(&t, 2);
    assert(idx2 >= 0);
    assert(t.p[idx2].mac_valid == false);
    int idx3 = plants_table_find_id(&t, 3);
    assert(idx3 >= 0);
    assert(t.p[idx3].mac_valid == true);
    assert(memcmp(t.p[idx3].mac, MAC_B, 6) == 0);
    int idx_mac_b = plants_table_find_mac(&t, MAC_B);
    assert(idx_mac_b == idx3);

    /* assign(1, NULL) unassigns */
    assert(plants_table_assign(&t, 1, NULL) == true);
    int idx1 = plants_table_find_id(&t, 1);
    assert(idx1 >= 0);
    assert(t.p[idx1].mac_valid == false);
    assert(plants_table_find_mac(&t, MAC_A) == -1);

    /* assign/rename/delete on unknown id fail */
    assert(plants_table_assign(&t, 99, MAC_A) == false);
    assert(plants_table_rename(&t, 99, "x") == false);
    assert(plants_table_delete(&t, 99) == false);

    /* rename bounds: 33-char name rejected, 32 accepted */
    char name32[PLANT_NAME_LEN + 1];
    memset(name32, 'x', PLANT_NAME_LEN);
    name32[PLANT_NAME_LEN] = '\0';
    assert(strlen(name32) == 32);

    char name33[PLANT_NAME_LEN + 2];
    memset(name33, 'x', PLANT_NAME_LEN + 1);
    name33[PLANT_NAME_LEN + 1] = '\0';
    assert(strlen(name33) == 33);

    assert(plants_table_rename(&t, 3, name33) == false);
    assert(plants_table_rename(&t, 3, name32) == true);
    idx3 = plants_table_find_id(&t, 3);
    assert(strcmp(t.p[idx3].name, name32) == 0);

    /* delete(2) retires: find_id(2) == -1, id 2 never reused */
    assert(plants_table_delete(&t, 2) == true);
    assert(plants_table_find_id(&t, 2) == -1);
    assert(t.next_id == 4); /* delete never touches next_id */

    uint8_t id_c = plants_table_resolve(&t, MAC_C);
    assert(id_c == 4);
    assert(t.next_id == 5);

    /* next_id persistence: always last id + 1 */
    assert(t.next_id == (uint8_t)(id_c + 1));

    /* currently in_use: ids 1, 3, 4 (id 2 retired) = 3 entries.
     * Fill remaining PLANTS_MAX - 3 slots to hit capacity. */
    uint8_t last_id = id_c;
    for (int i = 0; i < PLANTS_MAX - 3; i++) {
        uint8_t new_id = plants_table_create(&t, NULL);
        assert(new_id != 0);
        assert(new_id == (uint8_t)(last_id + 1));
        assert(t.next_id == (uint8_t)(new_id + 1));
        last_id = new_id;
    }

    /* table now full: create returns 0, next_id unchanged */
    assert(plants_table_create(&t, NULL) == 0);
    assert(t.next_id == (uint8_t)(last_id + 1));

    /* delete one -> create succeeds with a FRESH id (never id 4 again) */
    assert(plants_table_delete(&t, 4) == true);
    assert(plants_table_find_id(&t, 4) == -1);
    uint8_t fresh_id = plants_table_create(&t, NULL);
    assert(fresh_id != 0);
    assert(fresh_id != 4);
    assert(fresh_id == (uint8_t)(last_id + 1));
    assert(t.next_id == (uint8_t)(fresh_id + 1));

    printf("test_plants_table: OK\n");
    return 0;
}
