/* Host test for mapping_engine.c (pure). */
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "mapping_engine.h"
#include "dev_profiles.h"
#include "tuya_dp.h"
#include "capability.h"

int main(void) {
    /* --- standard measurement: suppressed cluster drops, others submit --- */
    uint16_t sup[] = { 0x0405 };
    assert(mapping_measurement(0x0405, sup, 1) == MAP_DROP);
    assert(mapping_measurement(0x0402, sup, 1) == MAP_SUBMIT);
    assert(mapping_measurement(0x0405, NULL, 0) == MAP_SUBMIT);   /* no suppress-set */
    assert(mapping_measurement(0, sup, 1) == MAP_SUBMIT);         /* cluster 0 never suppressed */

    /* --- Tuya DP: mapped -> submit converted; unmapped -> record --- */
    tuya_dp_map_t applied[2];
    memset(applied, 0, sizeof applied);
    applied[0].dp_id = 5; applied[0].cap_id = CAP_AIR_TEMPERATURE; applied[0].scale = 0.1f; applied[0].offset = 0.0f;
    uint8_t cap = 0xFF; float val = 0.0f;
    assert(mapping_dp(5, 256, applied, 1, &cap, &val) == MAP_SUBMIT);
    assert(cap == CAP_AIR_TEMPERATURE && fabsf(val - 25.6f) < 1e-4f);
    assert(mapping_dp(99, 10, applied, 1, &cap, &val) == MAP_RECORD_UNMAPPED);

    /* --- proposals: profile entries not yet in the applied map/suppress-set --- */
    const dev_profile_t *zs = dev_profile_match("_TZE284_o9ofysmo", "TS0601", NULL);
    assert(zs != NULL);
    dev_profile_entry_t props[DEV_PROFILE_MAX_ENTRIES];
    /* nothing applied -> all 5 entries proposed */
    int n = mapping_proposals(zs, NULL, 0, NULL, 0, props, DEV_PROFILE_MAX_ENTRIES);
    assert(n == 5);
    /* dp 3 already applied + 0x0405 already suppressed -> 3 remain */
    tuya_dp_map_t ap2[1]; memset(ap2, 0, sizeof ap2);
    ap2[0].dp_id = 3; ap2[0].cap_id = CAP_SOIL_MOISTURE; ap2[0].scale = 1.0f;
    uint16_t s2[] = { 0x0405 };
    n = mapping_proposals(zs, ap2, 1, s2, 1, props, DEV_PROFILE_MAX_ENTRIES);
    assert(n == 3);
    for (int i = 0; i < n; i++) {
        if (props[i].kind == DEV_PROFILE_KIND_DP) assert(props[i].dp_id != 3);
        if (props[i].kind == DEV_PROFILE_KIND_SUPPRESS) assert(props[i].source_cluster != 0x0405);
    }

    printf("mapping_engine ok\n");
    return 0;
}
