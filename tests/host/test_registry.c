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

    printf("test_registry: OK\n");
    return 0;
}
