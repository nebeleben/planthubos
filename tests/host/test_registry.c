#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "registry.h"

static mibeacon_t mk(uint8_t mac_last, uint8_t cnt, int16_t temp_dc)
{
    mibeacon_t m;
    memset(&m, 0, sizeof(m));
    uint8_t mac[6] = { 0xC4, 0x7C, 0x8D, 0x11, 0x22, mac_last };
    memcpy(m.mac, mac, 6);
    m.product_id = MIBEACON_PRODUCT_MIFLORA;
    m.frame_cnt = cnt;
    m.has_temp = true; m.temp_dc = temp_dc;
    return m;
}

int main(void)
{
    registry_t r;
    registry_init(&r);
    assert(registry_count(&r) == 0);

    /* new sensor */
    mibeacon_t m = mk(0x01, 1, 210);
    assert(registry_update(&r, &m, 100) == 1);
    assert(registry_count(&r) == 1);
    int idx = registry_find(&r, m.mac);
    assert(idx >= 0 && r.sensors[idx].last_seen_s == 100 && r.sensors[idx].latest.temp_dc == 210);

    /* same frame_cnt = duplicate: last_seen refreshed, no data change */
    m.temp_dc = 999;
    assert(registry_update(&r, &m, 130) == 0);
    assert(r.sensors[idx].latest.temp_dc == 210 && r.sensors[idx].last_seen_s == 130);

    /* new frame merges only the fields present */
    mibeacon_t m2 = mk(0x01, 2, 0);
    m2.has_temp = false;
    m2.has_moisture = true; m2.moisture_pct = 55;
    assert(registry_update(&r, &m2, 160) == 1);
    assert(r.sensors[idx].latest.temp_dc == 210);          /* kept */
    assert(r.sensors[idx].latest.has_temp);                 /* still flagged */
    assert(r.sensors[idx].latest.moisture_pct == 55 && r.sensors[idx].latest.has_moisture);
    assert(r.sensors[idx].latest.frame_cnt == 2);

    /* fill the table, 17th distinct MAC rejected */
    for (uint8_t i = 2; i <= REGISTRY_MAX_SENSORS; i++) {
        mibeacon_t x = mk(i, 1, 100);
        assert(registry_update(&r, &x, 200) == 1);
    }
    assert(registry_count(&r) == REGISTRY_MAX_SENSORS);
    mibeacon_t overflow = mk(0xFF, 1, 100);
    assert(registry_update(&r, &overflow, 210) == -1);
    assert(registry_find(&r, overflow.mac) == -1);

    /* --- attribution (M5b) --- */
    registry_t ra;
    registry_init(&ra);
    uint8_t nodeA[6] = { 0xAA,0,0,0,0,1 }, nodeB[6] = { 0xBB,0,0,0,0,2 };
    mibeacon_t x = mk(0x10, 1, 200);

    /* first sighting via node A at -70 */
    assert(registry_update_from(&ra, &x, 10, nodeA, -70) == 1);
    int i = registry_find(&ra, x.mac);
    assert(ra.sensors[i].via_node_valid && memcmp(ra.sensors[i].via_node, nodeA, 6) == 0);
    assert(ra.sensors[i].best_rssi == -70);

    /* same frame via node B, STRONGER -> B takes over, still a duplicate */
    assert(registry_update_from(&ra, &x, 11, nodeB, -40) == 0);
    assert(memcmp(ra.sensors[i].via_node, nodeB, 6) == 0 && ra.sensors[i].best_rssi == -40);

    /* same frame via node A again, WEAKER -> B keeps it */
    assert(registry_update_from(&ra, &x, 12, nodeA, -80) == 0);
    assert(memcmp(ra.sensors[i].via_node, nodeB, 6) == 0 && ra.sensors[i].best_rssi == -40);

    /* the hub hearing it directly always wins over any relay */
    assert(registry_update_from(&ra, &x, 13, NULL, 0) == 0);
    assert(!ra.sensors[i].via_node_valid);

    /* a NEW frame re-opens attribution: node A reports it first */
    mibeacon_t x2 = mk(0x10, 2, 210);
    assert(registry_update_from(&ra, &x2, 20, nodeA, -75) == 1);
    assert(ra.sensors[i].via_node_valid && memcmp(ra.sensors[i].via_node, nodeA, 6) == 0);
    assert(ra.sensors[i].best_rssi == -75);

    /* the old wrapper still behaves exactly as before */
    assert(registry_update(&ra, &x2, 21) == 0);

    /* --- registry_clear_attribution (M5b: forgetting a node) --- */
    registry_t rc;
    registry_init(&rc);
    uint8_t nodeC[6] = { 0xAA,0,0,0,0,10 }, nodeD[6] = { 0xBB,0,0,0,0,20 };

    /* two sensors attributed to nodeC, one to nodeD */
    mibeacon_t c1 = mk(0x21, 1, 100), c2 = mk(0x22, 1, 100), d1 = mk(0x23, 1, 100);
    assert(registry_update_from(&rc, &c1, 10, nodeC, -50) == 1);
    assert(registry_update_from(&rc, &c2, 10, nodeC, -55) == 1);
    assert(registry_update_from(&rc, &d1, 10, nodeD, -60) == 1);
    int ic1 = registry_find(&rc, c1.mac), ic2 = registry_find(&rc, c2.mac), id1 = registry_find(&rc, d1.mac);
    assert(rc.sensors[ic1].via_node_valid && rc.sensors[ic2].via_node_valid && rc.sensors[id1].via_node_valid);

    /* clearing nodeC affects exactly its two entries */
    assert(registry_clear_attribution(&rc, nodeC) == 2);
    assert(!rc.sensors[ic1].via_node_valid);
    assert(memcmp(rc.sensors[ic1].via_node, (uint8_t[6]){0}, 6) == 0);
    assert(rc.sensors[ic1].best_rssi == 0);
    assert(!rc.sensors[ic2].via_node_valid);
    assert(memcmp(rc.sensors[ic2].via_node, (uint8_t[6]){0}, 6) == 0);
    assert(rc.sensors[ic2].best_rssi == 0);

    /* nodeD's entry is untouched */
    assert(rc.sensors[id1].via_node_valid && memcmp(rc.sensors[id1].via_node, nodeD, 6) == 0);
    assert(rc.sensors[id1].best_rssi == -60);

    /* clearing an already-cleared/unknown node is a no-op */
    assert(registry_clear_attribution(&rc, nodeC) == 0);

    /* a subsequent reading re-attributes normally, exactly as if the entry
     * had never been attributed at all */
    mibeacon_t c1b = mk(0x21, 2, 110);
    assert(registry_update_from(&rc, &c1b, 30, nodeD, -45) == 1);
    assert(rc.sensors[ic1].via_node_valid && memcmp(rc.sensors[ic1].via_node, nodeD, 6) == 0);
    assert(rc.sensors[ic1].best_rssi == -45);

    /* --- registry_set_battery (M6: MiFlora battery poll) --- */
    registry_t rb;
    registry_init(&rb);
    uint8_t bmac[6] = { 0xC4, 0x7C, 0x8D, 0x33, 0x33, 0x01 };

    /* first time heard: creates the entry with no MiBeacon frame at all */
    assert(registry_set_battery(&rb, bmac, 42, 500) == 1);
    int ib = registry_find(&rb, bmac);
    assert(ib >= 0);
    assert(rb.sensors[ib].latest.has_battery && rb.sensors[ib].latest.battery_pct == 42);
    assert(rb.sensors[ib].last_seen_s == 500);
    assert(rb.sensors[ib].latest.frame_cnt == 0 && rb.sensors[ib].latest.product_id == 0);

    /* a later poll updates in place -- never treated as a dup even though
     * the entry's frame_cnt is (and stays) 0, unlike registry_update_from() */
    assert(registry_set_battery(&rb, bmac, 55, 900) == 1);
    assert(rb.sensors[ib].latest.battery_pct == 55 && rb.sensors[ib].last_seen_s == 900);

    /* battery-only updates never disturb frame_cnt/product_id/other fields */
    mibeacon_t full = mk(0x01, 7, 300);
    uint8_t fmac[6];
    memcpy(fmac, full.mac, 6);
    assert(registry_update(&rb, &full, 1000) == 1);
    int ifm = registry_find(&rb, fmac);
    assert(registry_set_battery(&rb, fmac, 61, 1010) == 1);
    assert(rb.sensors[ifm].latest.frame_cnt == 7);
    assert(rb.sensors[ifm].latest.product_id == MIBEACON_PRODUCT_MIFLORA);
    assert(rb.sensors[ifm].latest.has_temp && rb.sensors[ifm].latest.temp_dc == 300);
    assert(rb.sensors[ifm].latest.has_battery && rb.sensors[ifm].latest.battery_pct == 61);
    assert(rb.sensors[ifm].last_seen_s == 1010);

    /* registry full: a new mac is rejected and never created */
    registry_t rf;
    registry_init(&rf);
    for (uint8_t i = 1; i <= REGISTRY_MAX_SENSORS; i++) {
        mibeacon_t y = mk(i, 1, 100);
        assert(registry_update(&rf, &y, 100) == 1);
    }
    uint8_t newmac[6] = { 0xC4, 0x7C, 0x8D, 0x99, 0x99, 0x99 };
    assert(registry_set_battery(&rf, newmac, 50, 100) == -1);
    assert(registry_find(&rf, newmac) == -1);

    printf("test_registry: OK\n");
    return 0;
}
