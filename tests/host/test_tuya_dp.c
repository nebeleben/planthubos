/* tests/host/test_tuya_dp.c */
#include "tuya_dp.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static device_id_t mk_id(uint8_t last_byte) {
    device_id_t id;
    id.kind = DEV_KIND_ZIGBEE;
    memset(id.addr, 0, sizeof id.addr);
    id.addr[7] = last_byte;
    return id;
}

int main(void) {
    device_id_t a = mk_id(0xA1);
    device_id_t b = mk_id(0xB2);

    /* --- observed store: add/update, list returns latest value --- */
    tuya_dp_reset();
    tuya_dp_observe(&a, 1, 2, 100, 10);
    tuya_dp_obs_t out[TUYA_DP_MAX_PER_DEVICE];
    int n = tuya_dp_list(&a, out, TUYA_DP_MAX_PER_DEVICE);
    assert(n == 1);
    assert(out[0].dp_id == 1 && out[0].type == 2 && out[0].value == 100 && out[0].updated_s == 10);

    tuya_dp_observe(&a, 1, 2, 200, 20);   /* same dp_id -> updates in place */
    n = tuya_dp_list(&a, out, TUYA_DP_MAX_PER_DEVICE);
    assert(n == 1);
    assert(out[0].value == 200 && out[0].updated_s == 20);

    /* unknown device -> empty list */
    n = tuya_dp_list(&b, out, TUYA_DP_MAX_PER_DEVICE);
    assert(n == 0);

    /* --- per-device DP cap: a 13th distinct DP replaces the oldest --- */
    tuya_dp_reset();
    for (int i = 0; i < TUYA_DP_MAX_PER_DEVICE; i++)
        tuya_dp_observe(&a, (uint8_t)i, 0, i * 10, (uint32_t)(100 + i));
    n = tuya_dp_list(&a, out, TUYA_DP_MAX_PER_DEVICE);
    assert(n == TUYA_DP_MAX_PER_DEVICE);
    /* dp_id 0 has the oldest updated_s (100) -- one more distinct dp evicts it */
    tuya_dp_observe(&a, 99, 0, 999, 500);
    n = tuya_dp_list(&a, out, TUYA_DP_MAX_PER_DEVICE);
    assert(n == TUYA_DP_MAX_PER_DEVICE);
    bool saw_99 = false, saw_0 = false;
    for (int i = 0; i < n; i++) {
        if (out[i].dp_id == 99) saw_99 = true;
        if (out[i].dp_id == 0) saw_0 = true;
    }
    assert(saw_99 && !saw_0);

    /* --- device cap: a 9th device evicts the least-recently-active one --- */
    tuya_dp_reset();
    device_id_t devs[TUYA_DP_MAX_DEVICES + 1];
    for (int i = 0; i < TUYA_DP_MAX_DEVICES; i++) {
        devs[i] = mk_id((uint8_t)i);
        tuya_dp_observe(&devs[i], 0, 0, i, (uint32_t)(1000 + i));
    }
    devs[TUYA_DP_MAX_DEVICES] = mk_id(0xFF);
    tuya_dp_observe(&devs[TUYA_DP_MAX_DEVICES], 0, 0, 0, 5000);
    /* devs[0] had the oldest activity (1000) -- it's the one evicted */
    assert(tuya_dp_list(&devs[0], out, TUYA_DP_MAX_PER_DEVICE) == 0);
    assert(tuya_dp_list(&devs[TUYA_DP_MAX_DEVICES], out, TUYA_DP_MAX_PER_DEVICE) == 1);
    assert(tuya_dp_list(&devs[1], out, TUYA_DP_MAX_PER_DEVICE) == 1);   /* untouched */

    /* --- mapping: set/get/clear --- */
    tuya_dp_reset();
    uint8_t cap_id_out; float scale_out;
    assert(!tuya_dp_map_get(&a, 1, &cap_id_out, &scale_out));  /* unknown */

    assert(tuya_dp_map_set(&a, 1, CAP_AIR_TEMPERATURE, 0.1f));
    assert(tuya_dp_map_get(&a, 1, &cap_id_out, &scale_out));
    assert(cap_id_out == CAP_AIR_TEMPERATURE);
    assert(fabsf(scale_out - 0.1f) < 1e-6f);

    assert(tuya_dp_map_set(&a, 1, CAP_SOIL_MOISTURE, 0.5f)); /* replace in place */
    assert(tuya_dp_map_get(&a, 1, &cap_id_out, &scale_out));
    assert(cap_id_out == CAP_SOIL_MOISTURE);

    assert(tuya_dp_map_clear(&a, 1));
    assert(!tuya_dp_map_clear(&a, 1));  /* already gone */
    assert(!tuya_dp_map_get(&a, 1, &cap_id_out, &scale_out));

    /* invalid cap_id rejected -- table unchanged */
    assert(!tuya_dp_map_set(&a, 2, CAPABILITY_COUNT, 1.0f));
    assert(!tuya_dp_map_get(&a, 2, &cap_id_out, &scale_out));
    /* non-finite scale rejected too */
    assert(!tuya_dp_map_set(&a, 2, CAP_AIR_TEMPERATURE, (float)(1.0/0.0)));
    assert(!tuya_dp_map_get(&a, 2, &cap_id_out, &scale_out));

    /* --- mapping table: fill then a new (id,dp) is refused, existing updates --- */
    device_id_t map_ids[TUYA_MAP_MAX];
    for (int i = 0; i < TUYA_MAP_MAX; i++) {
        map_ids[i] = mk_id((uint8_t)(0x10 + i));
        assert(tuya_dp_map_set(&map_ids[i], 0, CAP_SOIL_MOISTURE, 1.0f));
    }
    device_id_t extra_id = mk_id(0xEE);
    assert(!tuya_dp_map_set(&extra_id, 0, CAP_SOIL_MOISTURE, 1.0f));  /* full */
    assert(tuya_dp_map_set(&map_ids[0], 0, CAP_BATTERY_LEVEL, 2.0f)); /* existing updates when full */
    assert(tuya_dp_map_get(&map_ids[0], 0, &cap_id_out, &scale_out));
    assert(cap_id_out == CAP_BATTERY_LEVEL);

    tuya_dp_map_t map_list[TUYA_MAP_MAX];
    assert(tuya_dp_map_list(map_list, TUYA_MAP_MAX) == TUYA_MAP_MAX);

    /* --- serialize -> deserialize round trip --- */
    uint8_t buf[512];
    size_t len = tuya_dp_map_serialize(buf, sizeof buf);
    assert(len > 0);
    assert(tuya_dp_map_deserialize(buf, len));
    assert(tuya_dp_map_list(map_list, TUYA_MAP_MAX) == TUYA_MAP_MAX);
    assert(tuya_dp_map_get(&map_ids[0], 0, &cap_id_out, &scale_out));
    assert(cap_id_out == CAP_BATTERY_LEVEL);
    assert(fabsf(scale_out - 2.0f) < 1e-6f);

    /* --- corrupted / truncated buffer deserializes to empty --- */
    uint8_t bad[512];
    memcpy(bad, buf, len);
    bad[len - 1] ^= 0xFF;
    assert(!tuya_dp_map_deserialize(bad, len));
    assert(tuya_dp_map_list(map_list, TUYA_MAP_MAX) == 0);

    /* re-populate, then re-check truncation on its own */
    assert(tuya_dp_map_set(&a, 1, CAP_AIR_TEMPERATURE, 0.1f));
    len = tuya_dp_map_serialize(buf, sizeof buf);
    assert(len > 0);
    assert(!tuya_dp_map_deserialize(buf, 1));
    assert(tuya_dp_map_list(map_list, TUYA_MAP_MAX) == 0);

    /* --- apply math is exact --- */
    assert(fabsf(tuya_dp_apply(256, 0.1f) - 25.6f) < 1e-4f);
    assert(fabsf(tuya_dp_apply(-50, 2.0f) - (-100.0f)) < 1e-4f);
    assert(fabsf(tuya_dp_apply(0, 3.7f) - 0.0f) < 1e-4f);
    /* offset-aware apply */
    assert(fabsf(tuya_dp_apply_off(256, 0.1f, 5.0f) - 30.6f) < 1e-4f);
    assert(fabsf(tuya_dp_apply_off(-50, 2.0f, -1.0f) - (-101.0f)) < 1e-4f);

    /* --- offset + provenance persist through the map --- */
    tuya_dp_map_deserialize((const uint8_t[]){ 0 }, 1);   /* reset -> empty */
    float off_out = -1.0f; uint8_t prov_out = 0xFF;
    assert(tuya_dp_map_set_ex(&a, 7, CAP_AIR_TEMPERATURE, 0.1f, 2.5f, TUYA_PROV_PROFILE));
    assert(tuya_dp_map_get_ex(&a, 7, &cap_id_out, &scale_out, &off_out, &prov_out));
    assert(fabsf(off_out - 2.5f) < 1e-4f && prov_out == TUYA_PROV_PROFILE);
    /* the 4-arg back-compat wrapper defaults provenance to manual */
    assert(tuya_dp_map_set(&a, 8, CAP_SOIL_MOISTURE, 1.0f));
    prov_out = 0xFF;
    assert(tuya_dp_map_get_ex(&a, 8, &cap_id_out, &scale_out, &off_out, &prov_out));
    assert(prov_out == TUYA_PROV_MANUAL);
    uint8_t sbuf[512];
    size_t slen = tuya_dp_map_serialize(sbuf, sizeof sbuf);
    assert(slen > 0 && sbuf[0] == 2);                     /* fmt 2 */
    off_out = -1.0f; prov_out = 0xFF;
    assert(tuya_dp_map_deserialize(sbuf, slen));
    assert(tuya_dp_map_get_ex(&a, 7, &cap_id_out, &scale_out, &off_out, &prov_out));
    assert(fabsf(off_out - 2.5f) < 1e-4f && prov_out == TUYA_PROV_PROFILE);

    /* --- suppress set/query/persist round trip (with provenance) --- */
    assert(!tuya_dp_suppressed(&a, 0x0405));
    assert(tuya_dp_suppress_set(&a, 0x0405, TUYA_PROV_AI));
    assert(tuya_dp_suppressed(&a, 0x0405));
    assert(!tuya_dp_suppressed(&b, 0x0405));              /* per device */
    prov_out = 0xFF;
    assert(tuya_dp_suppress_provenance(&a, 0x0405, &prov_out) && prov_out == TUYA_PROV_AI);
    slen = tuya_dp_map_serialize(sbuf, sizeof sbuf);
    assert(tuya_dp_map_deserialize(sbuf, slen));
    assert(tuya_dp_suppressed(&a, 0x0405));               /* survived persist */
    prov_out = 0xFF;
    assert(tuya_dp_suppress_provenance(&a, 0x0405, &prov_out) && prov_out == TUYA_PROV_AI);
    assert(tuya_dp_suppress_clear(&a, 0x0405));
    assert(!tuya_dp_suppressed(&a, 0x0405));

    /* --- fmt-1 back-compat: a fmt-1 image loads with offset defaulted to 0
     * and an empty suppress-set. Hand-forging a fmt-1 CRC is fiddly, so build
     * a fmt-2 image with the real serializer, downgrade it byte-for-byte to
     * fmt 1 with the test-only helper (drops the offset bytes + suppress
     * block, recomputes the CRC), then read it back. --- */
    {
        tuya_dp_map_deserialize((const uint8_t[]){ 0 }, 1);   /* reset -> empty */
        assert(tuya_dp_map_set_ex(&a, 9, CAP_SOIL_MOISTURE, 0.5f, 3.0f, TUYA_PROV_PROFILE));
        uint8_t v2[512];
        size_t v2len = tuya_dp_map_serialize(v2, sizeof v2);   /* fmt 2, with offset + provenance */
        assert(v2[0] == 2);
        uint8_t v1[512];
        size_t n1 = tuya_dp_map_downgrade_fmt1(v2, v2len, v1, sizeof v1);
        assert(n1 > 0 && v1[0] == 1);
        float o = 9.0f; uint8_t p = 0xFF;
        assert(tuya_dp_map_deserialize(v1, n1));
        assert(tuya_dp_map_get_ex(&a, 9, &cap_id_out, &scale_out, &o, &p));
        assert(fabsf(scale_out - 0.5f) < 1e-4f && o == 0.0f && p == TUYA_PROV_MANUAL); /* both defaulted */
    }

    printf("test_tuya_dp: OK\n");
    return 0;
}
