#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capability.h"

/* Hub-side Tuya EF00 datapoint store: what raw DPs a Zigbee device has
 * reported (RAM only, boot-scoped) and the operator's DP -> capability
 * mapping (persisted). Deliberately pure/host-testable, same split as
 * data_core/registry.c (RAM store) + wrappers/wrapper_bind.c (persisted
 * table): this component only stores and does the pure apply arithmetic.
 * swarm.c's bridge dispatch wires tuya_dp_observe() in, and the
 * /api/v1/devices/{id}/datapoints routes wire tuya_dp_map_set/clear() in
 * (both live outside this file -- this component takes no such dependency).
 *
 * device_id_t here is always DEV_KIND_ZIGBEE; nothing in this file enforces
 * that (a caller-side concern, same as registry.c not caring which kind it
 * stores).
 */

#define TUYA_DP_MAX_DEVICES    8
#define TUYA_DP_MAX_PER_DEVICE 12
#define TUYA_MAP_MAX           16
#define TUYA_DP_MAP_FMT        1

typedef struct {
    uint8_t  dp_id;
    uint8_t  type;       /* Tuya DP type byte (bool/value/string/enum/bitmap/raw) */
    int32_t  value;
    uint32_t updated_s;
} tuya_dp_obs_t;

typedef struct {
    device_id_t id;
    uint8_t     dp_id;
    uint8_t     cap_id;
    float       scale;
} tuya_dp_map_t;

/* --- Observed store (RAM only, cleared at boot) ---
 * Bounded TUYA_DP_MAX_DEVICES devices, TUYA_DP_MAX_PER_DEVICE distinct DPs
 * each. A device beyond the device cap evicts the least-recently-active
 * device (LRU by last observe, mirroring registry.c's device-slot
 * eviction); a DP beyond the per-device cap evicts that device's oldest DP
 * (by updated_s) rather than the whole device. */
void tuya_dp_reset(void);
void tuya_dp_observe(const device_id_t *id, uint8_t dp_id, uint8_t type,
                      int32_t value, uint32_t now_s);
/* Copies up to max observed DPs for id into out (undefined order beyond
 * "most recently replaced slots first" is not guaranteed); returns the
 * count copied, 0 if id is unknown. */
int tuya_dp_list(const device_id_t *id, tuya_dp_obs_t *out, int max);

/* --- Mapping (persisted) ---
 * TUYA_MAP_MAX entries total across all devices, keyed by (id, dp_id).
 * tuya_dp_map_set validates cap_id < CAPABILITY_COUNT and that scale is
 * finite; it neither writes nor clears the map on rejection. */
bool tuya_dp_map_set(const device_id_t *id, uint8_t dp_id, uint8_t cap_id, float scale);
bool tuya_dp_map_clear(const device_id_t *id, uint8_t dp_id);
bool tuya_dp_map_get(const device_id_t *id, uint8_t dp_id, uint8_t *cap_id_out, float *scale_out);
int  tuya_dp_map_list(tuya_dp_map_t *out, int max);

/* Pure apply arithmetic: DP raw value -> the capability's own unit, given
 * (value, scale) from tuya_dp_map_get(). Exact float multiply, no
 * rounding/clamping -- capability_encode() (capability.h) does the
 * storage-side scaling from there. swarm.c's dispatch inlines this same
 * `(float)value * scale`; this helper is its host-testable expression and
 * is available to any caller that prefers it over the literal. */
float tuya_dp_apply(int32_t value, float scale);

/* File format (little-endian), CRC-16/CCITT-FALSE over every byte but the
 * crc field: [0]=fmt(=1) [1..2]=crc [3]=count, then count * {
 * id.kind(1), id.addr[8], dp_id(1), cap_id(1), scale as 4 raw float bytes }.
 * Mirrors wrapper_bind.c's layout discipline verbatim. */
size_t tuya_dp_map_serialize(uint8_t *buf, size_t cap);
/* Resets the map table first (same as wrapper_bind_deserialize): a
 * corrupt/truncated/wrong-fmt buffer leaves the map empty and returns
 * false, never a partially-applied table. */
bool   tuya_dp_map_deserialize(const uint8_t *buf, size_t len);

#ifdef ESP_PLATFORM
/* Load the persisted map at boot (no-op leaving an empty map if the file is
 * absent or corrupt). Save atomically (tmp+rename) after any change. */
void tuya_dp_map_load(void);
bool tuya_dp_map_save(void);
#endif
