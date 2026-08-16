#pragma once
#include "cJSON.h"
#include "registry.h"
#include "plants_table.h"

/* Task 6 (M2 spec Sec.6/Sec.7): device+capability JSON builders. Renamed
 * from sensors_json.h/.c (kept the CMakeLists SRCS churn to a rename, not a
 * new file) now that the primary registry shape is registry.h's
 * device_entry_t rather than V1's mac-keyed sensor_entry_t/mibeacon_t.
 * Two builders, one file, same "shared JSON builder for both plant and
 * device JSON" convention the pre-rename file used:
 *
 *   - device_json() -- shared by every surface that renders a live
 *     device: GET /api/v1/devices, its deprecated GET /api/v1/sensors
 *     alias (both in api_v1.c), and sse.c's on_sensor_update() push. All
 *     three emit the SAME per-device shape (task-6-brief.md's route
 *     table): {id, kind, last_seen_s, via, rssi, name, caps:[...],
 *     plant_ids:[...]} -- "id"/"kind" are the device_id_t's text form and
 *     kind_prefix() word (capability.h's device_id_format()/
 *     device_id_t.kind); "caps" is one entry per currently-valid
 *     capability slot, metadata from capability.h; "plant_ids" is every
 *     plant currently binding ANY capability of this device
 *     (plants_table.h's cap_bound/cap_dev).
 *   - plant_json() -- GET /api/v1/plants' row: {id, name, bindings:[...]},
 *     one bindings[] entry per currently-bound capability, value/age_s via
 *     plants_cap_value() (plants.h).
 */

/* Builds one device's JSON object. `plants` is the caller's current plants
 * snapshot, used only to populate "plant_ids" -- pass NULL to skip that
 * computation and emit an empty "plant_ids" array instead (sse.c's
 * on_sensor_update() does this: it runs on the default event-loop task,
 * which has only a 2304B stack and takes no plants snapshot of its own).
 * now_uptime_s is esp_timer uptime seconds (esp_timer_get_time()/1e6),
 * used for "last_seen_s" and each capability's "age_s". */
cJSON *device_json(const device_entry_t *e, const plants_table_t *plants, uint32_t now_uptime_s);

/* Builds one plant's JSON object: {id, name, bindings:[...]}. `reg` is the
 * caller's current registry snapshot (data_core_snapshot()), passed straight
 * through to plants_cap_value() (plants.h) for each bound capability's
 * live value/age_s -- null/null when that call can't produce one yet (the
 * bound device isn't in `reg`, or its slot has never been written). */
cJSON *plant_json(const plant_entry_t *p, const registry_t *reg);
