#pragma once
#include "cJSON.h"
#include "registry.h"
#include "plants_table.h"

/* M8 Task 6: plants are the primary surface now, so this file (kept under
 * its original name to avoid churning the CMakeLists SRCS list and sse.c's
 * #include for a single sensor-json.c dependency) is the ONE shared JSON
 * builder for every shape that renders a registry sensor_entry_t or a
 * plant_entry_t -- three of them, per the task brief's "keep ONE builder
 * file for both plant and probe JSON":
 *
 *   - sensor_json()  -- the original full per-sensor shape (readings +
 *     name + via), UNCHANGED. Only remaining caller is sse.c's live push
 *     (on_sensor_update()) -- that channel is keyed by mac and out of this
 *     task's route contract, so it keeps its pre-M8 shape rather than being
 *     folded into the demoted GET /api/v1/sensors shape below.
 *   - probe_json()   -- GET /api/v1/sensors' new demoted row: diagnostics
 *     only (mac/battery/rssi/via/last_seen_s/plant_id), no readings, no
 *     name. Per-sensor history and rename are gone (spec §4); a sensor is
 *     just a probe now.
 *   - plant_json()   -- GET /api/v1/plants' row: the primary entity.
 */

/* Builds the JSON object for one sensor, as served by SSE
 * (webserver/sse.c's on_sensor_update()). Historical full shape, unchanged
 * by M8: {mac, name, temp, moisture, lux, conductivity, battery,
 * last_seen_s, via}. "via" (M5b, attribution): null when the hub heard this
 * sensor on its own BLE radio directly; otherwise {"mac":...,"name":...,
 * "rssi":...} -- the node registry_update_from()'s "strongest rssi wins"
 * rule currently attributes this sensor's data to (see registry.h). */
cJSON *sensor_json(const sensor_entry_t *e);

/* Builds the JSON object for one row of the demoted GET /api/v1/sensors
 * (spec §4's "probe pool"): {mac, battery, rssi, via, last_seen_s,
 * plant_id}. last_seen_s is ALWAYS an age in seconds (now_uptime_s -
 * e->last_seen_s, both esp_timer uptime seconds, clamped at 0 -- same fix
 * spec §4 calls out for the old raw-timestamp wart, mirroring
 * swarm_node_list_json()'s identical age conversion in swarm.c).
 * plant_id is 0 when this mac isn't assigned to any plant (never a valid
 * id, plants_table.h) -- rendered as JSON null. */
cJSON *probe_json(const sensor_entry_t *e, uint8_t plant_id, uint32_t now_uptime_s);

/* Builds the JSON object for one row of GET /api/v1/plants: {id, name,
 * temp, moisture, lux, conductivity, battery, last_seen_s, probe}.
 *
 * Values come from `snap` (the current registry_find() lookup of the
 * plant's assigned mac) whenever that lookup succeeds -- last_seen_s is
 * then an age off now_uptime_s, same convention as probe_json(). Otherwise
 * (no assigned mac, or an assigned mac the radio has never actually heard
 * yet -- see the probe-assignment route's "pre-assign a replacement probe"
 * case) values fall back to plants_last_values()'s ring-tail read, with
 * last_seen_s an age off now_epoch (wall-clock, since the cached record's
 * timestamp is itself a resolved epoch, not an uptime value) -- null
 * throughout when there is no history at all yet.
 *
 * "probe" mirrors the probe pool's diagnostics shape (mac/battery/rssi/via)
 * minus last_seen_s/plant_id (redundant at this nesting level): null when
 * the plant has no assigned mac; present-with-nulled-diagnostics when a mac
 * is assigned but not yet live in the registry (the "pending" pre-assigned
 * case above); the live values otherwise. */
cJSON *plant_json(const plant_entry_t *p, const registry_t *snap,
                   uint32_t now_uptime_s, uint32_t now_epoch);
