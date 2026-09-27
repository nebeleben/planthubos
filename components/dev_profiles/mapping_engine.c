#include "mapping_engine.h"
#include <math.h>

#define MAP_FLOAT_EPS 1e-6f

map_action_t mapping_measurement(uint16_t source_cluster,
                                 const uint16_t *suppress_set, int suppress_count) {
    if (source_cluster != 0 && suppress_set) {
        for (int i = 0; i < suppress_count; i++)
            if (suppress_set[i] == source_cluster) return MAP_DROP;
    }
    return MAP_SUBMIT;
}

map_action_t mapping_dp(uint8_t dp_id, int32_t value,
                        const tuya_dp_map_t *applied, int applied_count,
                        uint8_t *cap_id_out, float *value_out) {
    for (int i = 0; i < applied_count; i++) {
        if (applied[i].dp_id == dp_id) {
            if (cap_id_out) *cap_id_out = applied[i].cap_id;
            if (value_out) *value_out = tuya_dp_apply_off(value, applied[i].scale, applied[i].offset);
            return MAP_SUBMIT;
        }
    }
    return MAP_RECORD_UNMAPPED;
}

/* True when this profile DP entry is already reflected verbatim in the
 * applied map: same dp_id AND same cap_id/scale/offset (spec section 4 --
 * a same-dp_id entry that differs is a correction and must still be
 * proposed, not withheld). */
static bool dp_entry_already_applied(const dev_profile_entry_t *e,
                                     const tuya_dp_map_t *applied, int applied_count) {
    for (int i = 0; i < applied_count; i++) {
        if (applied[i].dp_id != e->dp_id) continue;
        if (applied[i].cap_id != e->cap_id) return false;
        if (fabsf(applied[i].scale - e->scale) > MAP_FLOAT_EPS) return false;
        if (fabsf(applied[i].offset - e->offset) > MAP_FLOAT_EPS) return false;
        return true;
    }
    return false;
}

static bool cluster_is_suppressed(uint16_t c, const uint16_t *suppress_set, int suppress_count) {
    if (!suppress_set) return false;
    for (int i = 0; i < suppress_count; i++) if (suppress_set[i] == c) return true;
    return false;
}

int mapping_proposals(const dev_profile_t *p,
                      const tuya_dp_map_t *applied, int applied_count,
                      const uint16_t *suppress_set, int suppress_count,
                      dev_profile_entry_t *out, int max) {
    if (!p || !out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < p->entry_count && n < max; i++) {
        const dev_profile_entry_t *e = &p->entries[i];
        if (e->kind == DEV_PROFILE_KIND_DP) {
            if (!dp_entry_already_applied(e, applied, applied_count)) out[n++] = *e;
        } else if (e->kind == DEV_PROFILE_KIND_SUPPRESS) {
            if (!cluster_is_suppressed(e->source_cluster, suppress_set, suppress_count)) out[n++] = *e;
        }
    }
    return n;
}
