#pragma once
#include <stdint.h>
#include "dev_profiles.h"
#include "tuya_dp.h"

/* Pure ingest/proposal decisions (spec section 4). No I/O, no ESP-IDF, no
 * cJSON: the caller (swarm.c, api_v1.c) supplies the applied dp-map, the
 * suppress-set and the matched profile; this decides submit/drop/record and
 * which profile entries are not yet applied. */

typedef enum {
    MAP_SUBMIT = 0,          /* forward to data_core */
    MAP_DROP = 1,            /* suppressed -- never reaches the registry */
    MAP_RECORD_UNMAPPED = 2, /* observed but not mapped -- record for UI/AI */
} map_action_t;

/* Standard-cluster measurement: DROP when source_cluster (!=0) is in the
 * suppress-set, else SUBMIT. Cluster 0 is never suppressed. */
map_action_t mapping_measurement(uint16_t source_cluster,
                                 const uint16_t *suppress_set, int suppress_count);

/* Tuya EF00 datapoint: if dp_id is in the applied map, SUBMIT with cap_id_out
 * set and value_out = value*scale + offset; else RECORD_UNMAPPED. */
map_action_t mapping_dp(uint8_t dp_id, int32_t value,
                        const tuya_dp_map_t *applied, int applied_count,
                        uint8_t *cap_id_out, float *value_out);

/* Profile entries not yet reflected in the applied map (DP entries whose dp_id
 * is unmapped) or the suppress-set (suppress entries whose cluster is not yet
 * suppressed). Returns the count written to out (capped at max). */
int mapping_proposals(const dev_profile_t *p,
                      const tuya_dp_map_t *applied, int applied_count,
                      const uint16_t *suppress_set, int suppress_count,
                      dev_profile_entry_t *out, int max);
