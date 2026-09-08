#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bridge_table.h"

int main(void)
{
    bridge_table_t t; bridge_table_init(&t);
    uint8_t m1[6] = {1,1,1,1,1,1}, m2[6] = {2,2,2,2,2,2};
    swarm_device_announce_t a = { .dev = { .kind = 2, .addr = {1,2,3,4,5,6,7,8} }, .endpoint = 1, .interviewed = 1,
                                  .name_len = 1, .name = "A", .cap_count = 1, .cap_ids = {2}, .cap_clusters = {0x400} };
    assert(bridge_table_upsert(&t, m1, &a));
    assert(bridge_table_node(&t, m1, false)->count == 1);
    a.name_len = 2; a.name[1] = 'B';
    assert(bridge_table_upsert(&t, m1, &a));                       /* same device: update, no growth */
    assert(bridge_table_node(&t, m1, false)->count == 1 && bridge_table_node(&t, m1, false)->dev[0].name_len == 2);
    const bridge_node_t *owner = bridge_table_find_device(&t, &a.dev);
    assert(owner && memcmp(owner->mac, m1, 6) == 0);
    assert(bridge_table_remove(&t, m1, &a.dev) && bridge_table_node(&t, m1, false)->count == 0);
    assert(!bridge_table_remove(&t, m1, &a.dev));
    for (int i = 0; i < BRIDGE_MAX_DEVICES; i++) { a.dev.addr[0] = (uint8_t)i; assert(bridge_table_upsert(&t, m2, &a)); }
    a.dev.addr[0] = 0xEE; assert(!bridge_table_upsert(&t, m2, &a));   /* full */
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) { uint8_t m[6] = {9,9,9,9,9,(uint8_t)i}; bridge_table_node(&t, m, true); }
    uint8_t mx[6] = {7,7,7,7,7,7}; assert(bridge_table_node(&t, mx, true) == NULL);   /* node slots full */
    uint8_t buf[BRIDGE_MAX_NODES * (8 + BRIDGE_MAX_DEVICES * 64) + 16];
    size_t n = bridge_table_serialize(&t, buf, sizeof buf); assert(n > 0);
    bridge_table_t u; assert(bridge_table_deserialize(&u, buf, n));
    assert(bridge_table_node(&u, m2, false)->count == BRIDGE_MAX_DEVICES);
    assert(!bridge_table_deserialize(&u, buf, n - 1));
    bridge_table_forget_node(&t, m2); assert(bridge_table_node(&t, m2, false) == NULL);
    printf("test_bridge_table: OK\n");
    return 0;
}
