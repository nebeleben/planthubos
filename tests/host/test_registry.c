#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "registry.h"

static device_id_t mk_id(device_kind_t kind, uint8_t mac_last)
{
    uint8_t mac[6] = { 0xC4, 0x7C, 0x8D, 0x11, 0x22, mac_last };
    return device_id_from_mac(kind, mac);
}

int main(void)
{
    /* --- create-on-first-set, registry_count growth --- */
    registry_t r;
    registry_init(&r);
    assert(registry_count(&r) == 0);

    device_id_t id1 = mk_id(DEV_KIND_BLE, 0x01);
    assert(registry_find(&r, &id1) == -1);

    int idx = registry_set_cap(&r, &id1, CAP_AIR_TEMPERATURE, 210, 100);
    assert(idx >= 0);
    assert(registry_count(&r) == 1);
    assert(registry_find(&r, &id1) == idx);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].valid);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].raw == 210);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].updated_s == 100);
    assert(r.devices[idx].last_seen_s == 100);

    /* a second set_cap on the SAME id finds, does not re-create */
    int idx2 = registry_set_cap(&r, &id1, CAP_SOIL_MOISTURE, 55, 150);
    assert(idx2 == idx);
    assert(registry_count(&r) == 1);

    /* --- registry_find by id, incl. kind mismatch missing --- */
    device_id_t id1_espnow = mk_id(DEV_KIND_ESPNOW, 0x01);   /* same 6 mac bytes, different kind */
    assert(registry_find(&r, &id1_espnow) == -1);
    assert(registry_find(&r, &id1) == idx);   /* the real one is still there */

    /* --- two capabilities on one device keep both slots, own updated_s --- */
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].valid);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].raw == 210);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].updated_s == 100);   /* untouched by the moisture write */
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].valid);
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].raw == 55);
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].updated_s == 150);
    assert(r.devices[idx].last_seen_s == 150);   /* refreshed by the most recent write */

    /* --- CAP_VALUE_NONE clears a slot without deleting the device --- */
    int idx3 = registry_set_cap(&r, &id1, CAP_AIR_TEMPERATURE, CAP_VALUE_NONE, 200);
    assert(idx3 == idx);
    assert(!r.devices[idx].caps[CAP_AIR_TEMPERATURE].valid);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].raw == CAP_VALUE_NONE);
    assert(r.devices[idx].caps[CAP_AIR_TEMPERATURE].updated_s == 200);
    /* device itself, and its other capability, are untouched */
    assert(registry_count(&r) == 1);
    assert(r.devices[idx].in_use);
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].valid);
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].raw == 55);

    /* --- table full returns -1, does not corrupt existing entries --- */
    for (uint8_t i = 2; i <= REGISTRY_MAX_DEVICES; i++) {
        device_id_t id = mk_id(DEV_KIND_BLE, i);
        assert(registry_set_cap(&r, &id, CAP_SOIL_MOISTURE, 42, 300) >= 0);
    }
    assert(registry_count(&r) == REGISTRY_MAX_DEVICES);
    device_id_t overflow = mk_id(DEV_KIND_BLE, 0xFF);
    assert(registry_set_cap(&r, &overflow, CAP_SOIL_MOISTURE, 1, 400) == -1);
    assert(registry_find(&r, &overflow) == -1);
    assert(registry_count(&r) == REGISTRY_MAX_DEVICES);
    /* the very first entry is unchanged by the failed insert */
    assert(r.devices[idx].caps[CAP_SOIL_MOISTURE].raw == 55);
    assert(!r.devices[idx].caps[CAP_AIR_TEMPERATURE].valid);

    /* --- attribution (M5b) --- */
    registry_t ra;
    registry_init(&ra);
    uint8_t nodeA[6] = { 0xAA,0,0,0,0,1 }, nodeB[6] = { 0xBB,0,0,0,0,2 };
    device_id_t x = mk_id(DEV_KIND_BLE, 0x10);

    /* a brand-new device is unconditionally attributed to its first reporter */
    assert(registry_attribute(&ra, &x, 1, nodeA, -70, 10) == true);
    int i = registry_find(&ra, &x);
    assert(i >= 0);
    assert(ra.devices[i].via_node_valid && memcmp(ra.devices[i].via_node, nodeA, 6) == 0);
    assert(ra.devices[i].best_rssi == -70);
    assert(ra.devices[i].last_seen_s == 10);

    /* same frame via node B, STRONGER -> B takes over (owns) */
    assert(registry_attribute(&ra, &x, 1, nodeB, -40, 11) == true);
    assert(memcmp(ra.devices[i].via_node, nodeB, 6) == 0 && ra.devices[i].best_rssi == -40);

    /* same frame via node A again, WEAKER -> does not take ownership, B keeps it */
    assert(registry_attribute(&ra, &x, 1, nodeA, -80, 12) == false);
    assert(memcmp(ra.devices[i].via_node, nodeB, 6) == 0 && ra.devices[i].best_rssi == -40);
    assert(ra.devices[i].last_seen_s == 12);   /* last_seen still refreshes on a losing bid */

    /* the hub hearing it directly always wins over any relay */
    assert(registry_attribute(&ra, &x, 1, NULL, 0, 13) == true);
    assert(!ra.devices[i].via_node_valid);

    /* a NEW frame_cnt re-opens attribution: node A reports it first */
    assert(registry_attribute(&ra, &x, 2, nodeA, -75, 20) == true);
    assert(ra.devices[i].via_node_valid && memcmp(ra.devices[i].via_node, nodeA, 6) == 0);
    assert(ra.devices[i].best_rssi == -75);

    /* duplicate of that new frame, weaker via a different node -> no takeover */
    assert(registry_attribute(&ra, &x, 2, nodeB, -90, 21) == false);
    assert(memcmp(ra.devices[i].via_node, nodeA, 6) == 0);

    /* --- registry_clear_attribution (M5b: forgetting a node) --- */
    registry_t rc;
    registry_init(&rc);
    uint8_t nodeC[6] = { 0xAA,0,0,0,0,10 }, nodeD[6] = { 0xBB,0,0,0,0,20 };

    device_id_t c1 = mk_id(DEV_KIND_BLE, 0x21), c2 = mk_id(DEV_KIND_BLE, 0x22), d1 = mk_id(DEV_KIND_BLE, 0x23);
    assert(registry_attribute(&rc, &c1, 1, nodeC, -50, 10) == true);
    assert(registry_attribute(&rc, &c2, 1, nodeC, -55, 10) == true);
    assert(registry_attribute(&rc, &d1, 1, nodeD, -60, 10) == true);
    int ic1 = registry_find(&rc, &c1), ic2 = registry_find(&rc, &c2), id1_idx = registry_find(&rc, &d1);
    assert(rc.devices[ic1].via_node_valid && rc.devices[ic2].via_node_valid && rc.devices[id1_idx].via_node_valid);

    /* clearing nodeC affects exactly its two entries */
    registry_clear_attribution(&rc, nodeC);
    assert(!rc.devices[ic1].via_node_valid);
    assert(memcmp(rc.devices[ic1].via_node, (uint8_t[6]){0}, 6) == 0);
    assert(rc.devices[ic1].best_rssi == 0);
    assert(!rc.devices[ic2].via_node_valid);
    assert(memcmp(rc.devices[ic2].via_node, (uint8_t[6]){0}, 6) == 0);
    assert(rc.devices[ic2].best_rssi == 0);

    /* nodeD's entry is untouched */
    assert(rc.devices[id1_idx].via_node_valid && memcmp(rc.devices[id1_idx].via_node, nodeD, 6) == 0);
    assert(rc.devices[id1_idx].best_rssi == -60);

    /* clearing an already-cleared/unknown node is a no-op */
    registry_clear_attribution(&rc, nodeC);
    assert(!rc.devices[ic1].via_node_valid);

    /* a subsequent reading re-attributes normally, exactly as if the entry
     * had never been attributed at all */
    assert(registry_attribute(&rc, &c1, 2, nodeD, -45, 30) == true);
    assert(rc.devices[ic1].via_node_valid && memcmp(rc.devices[ic1].via_node, nodeD, 6) == 0);

    /* --- out-of-range encode must not erase a previously-good value ---
     *
     * data_core.c's submit_locked()/data_core_submit_battery() guard every
     * capability write: when capability_encode() returns CAP_VALUE_NONE for
     * a value outside that capability's encodable range (e.g. MiFlora's
     * uint16 conductivity_us, wire range 0-65535, can exceed
     * CAP_SOIL_CONDUCTIVITY's int16 ceiling of 32767), the fix is to skip
     * the registry_set_cap() call entirely rather than pass CAP_VALUE_NONE
     * through -- registry_set_cap(..., CAP_VALUE_NONE, ...) is a distinct,
     * deliberate "caller explicitly wants this slot cleared" contract (see
     * the CAP_VALUE_NONE-clears-a-slot test above) that must keep working
     * for callers that mean it. data_core.c itself isn't host-testable (it
     * needs FreeRTOS/esp_event), so this exercises the real
     * capability_encode() -- linked into this test binary already -- and
     * asserts the registry-level invariant that guard relies on: a
     * registry_set_cap() call that is never made cannot change stored
     * state. */
    registry_t rg;
    registry_init(&rg);
    device_id_t g1 = mk_id(DEV_KIND_BLE, 0x30);
    assert(registry_set_cap(&rg, &g1, CAP_SOIL_CONDUCTIVITY, 5000, 10) >= 0);
    int gidx = registry_find(&rg, &g1);
    assert(gidx >= 0);
    assert(rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].valid);
    assert(rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].raw == 5000);

    /* 40000 uS/cm is a legal MiFlora wire value (fits uint16) but exceeds
     * CAP_SOIL_CONDUCTIVITY's int16/scale-1 ceiling of 32767. */
    int16_t bad_raw = capability_encode(CAP_SOIL_CONDUCTIVITY, 40000.0f);
    assert(bad_raw == CAP_VALUE_NONE);
    if (bad_raw != CAP_VALUE_NONE) {
        /* Unreachable given the assert above; written to mirror
         * set_cap_or_warn()'s actual guard shape in data_core.c exactly. */
        registry_set_cap(&rg, &g1, CAP_SOIL_CONDUCTIVITY, bad_raw, 20);
    }
    /* the guard skipped the call above -- the good value from t=10 survives */
    assert(rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].valid);
    assert(rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].raw == 5000);
    assert(rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].updated_s == 10);

    /* a deliberate clear (a real caller passing CAP_VALUE_NONE on purpose,
     * e.g. a device that stopped reporting a capability) still works --
     * this is registry_set_cap()'s own contract, untouched by the guard. */
    assert(registry_set_cap(&rg, &g1, CAP_SOIL_CONDUCTIVITY, CAP_VALUE_NONE, 30) == gidx);
    assert(!rg.devices[gidx].caps[CAP_SOIL_CONDUCTIVITY].valid);
    assert(rc.devices[ic1].best_rssi == -45);

    printf("test_registry: OK\n");
    return 0;
}
