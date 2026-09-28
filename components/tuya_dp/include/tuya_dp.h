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
#define TUYA_SUPPRESS_MAX      16
#define TUYA_DP_MAP_FMT        2   /* v2 adds per-entry offset + provenance + a suppress-set block */

/* Where an applied mapping came from (device-mapping-profiles §3 provenance),
 * folded into the fmt-2 bump -- no extra format version. */
enum { TUYA_PROV_MANUAL = 0, TUYA_PROV_PROFILE = 1, TUYA_PROV_AI = 2 };

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
    float       offset;      /* v2: applied value = value*scale + offset */
    uint8_t     provenance;  /* v2: TUYA_PROV_* */
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

/* Offset-/provenance-aware variants (device-mapping-profiles). The original
 * 4-arg tuya_dp_map_set / 4-arg tuya_dp_map_get and 2-arg tuya_dp_apply are
 * kept as back-compat wrappers (offset 0, provenance TUYA_PROV_MANUAL). */
bool tuya_dp_map_set_ex(const device_id_t *id, uint8_t dp_id, uint8_t cap_id, float scale, float offset, uint8_t provenance);
bool tuya_dp_map_get_ex(const device_id_t *id, uint8_t dp_id, uint8_t *cap_id_out, float *scale_out, float *offset_out, uint8_t *provenance_out);
float tuya_dp_apply_off(int32_t value, float scale, float offset);

/* Per-device suppress-set: a source_cluster whose standard-cluster
 * measurements the hub must drop (the ZS-301Z soil-on-0x0405 flap). Each entry
 * carries its provenance. Persisted in the same map file (fmt 2). */
bool tuya_dp_suppress_set(const device_id_t *id, uint16_t source_cluster, uint8_t provenance);
bool tuya_dp_suppress_clear(const device_id_t *id, uint16_t source_cluster);
bool tuya_dp_suppressed(const device_id_t *id, uint16_t source_cluster);
int  tuya_dp_suppress_list(const device_id_t *id, uint16_t *out, int max);
bool tuya_dp_suppress_provenance(const device_id_t *id, uint16_t source_cluster, uint8_t *provenance_out);

/* Pure apply arithmetic: DP raw value -> the capability's own unit, given
 * (value, scale) from tuya_dp_map_get(). Exact float multiply, no
 * rounding/clamping -- capability_encode() (capability.h) does the
 * storage-side scaling from there. swarm.c's dispatch inlines this same
 * `(float)value * scale`; this helper is its host-testable expression and
 * is available to any caller that prefers it over the literal. */
float tuya_dp_apply(int32_t value, float scale);

/* File format (little-endian), CRC-16/CCITT-FALSE over every byte but the
 * crc field: [0]=fmt(1 or 2) [1..2]=crc [3]=count, then count * {
 * id.kind(1), id.addr[8], dp_id(1), cap_id(1), scale as 4 raw float bytes,
 * [fmt 2 only: offset as 4 raw float bytes, provenance(1)] }, then [fmt 2
 * only: sup_count(1), then sup_count * { id.kind(1), id.addr[8],
 * cluster(2 LE), provenance(1) }]. Mirrors wrapper_bind.c's layout
 * discipline verbatim. A fmt-1 file still reads (offset=0,
 * provenance=TUYA_PROV_MANUAL, empty suppress-set). */
size_t tuya_dp_map_serialize(uint8_t *buf, size_t cap);
/* Resets the map table first (same as wrapper_bind_deserialize): a
 * corrupt/truncated/wrong-fmt buffer leaves the map empty and returns
 * false, never a partially-applied table. */
bool   tuya_dp_map_deserialize(const uint8_t *buf, size_t len);

/* Test-only helper (host build): rewrite a fmt-2 image as a fmt-1 image
 * (drop offsets + suppress block) so the fmt-1 read path can be exercised. */
size_t tuya_dp_map_downgrade_fmt1(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap);

#ifdef ESP_PLATFORM
/* Load the persisted map at boot (no-op leaving an empty map if the file is
 * absent or corrupt). Save atomically (tmp+rename) after any change. */
void tuya_dp_map_load(void);
bool tuya_dp_map_save(void);
#endif
