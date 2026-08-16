#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include "registry.h"
#include "capability.h"
#include "mibeacon.h"
#include <stdbool.h>
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(PLANTHUB_DATA_EVENT);
enum { DATA_EVENT_SENSOR_UPDATE };

/* A relayed reading older than this is dropped outright rather than
 * resurrected as "current": a node that buffered through a long outage
 * should not make a stale value look live. */
#define DATA_CORE_MAX_AGE_S 1800

esp_err_t data_core_init(void);

/* The MiFlora -> capability adapter (M2): maps each has_* field of *m onto
 * its capability slot via capability_encode() (temp -> CAP_AIR_TEMPERATURE,
 * moisture -> CAP_SOIL_MOISTURE, lux -> CAP_LIGHT_ILLUMINANCE, conductivity
 * -> CAP_SOIL_CONDUCTIVITY, battery -> CAP_BATTERY_LEVEL), plus
 * CAP_SIGNAL_RSSI from rssi -- but only once registry_attribute() (M5b
 * rules, registry.h) says this reporter actually owns the frame; a losing
 * arbitration bid writes nothing, same as V1's duplicate-frame branch never
 * called merge(). via_node/rssi mean exactly what they did pre-M2: NULL is
 * a direct hub BLE reception, otherwise the relaying node's ESP-NOW MAC and
 * its rssi reading of the sensor. Uses "now" (esp_timer uptime) as the
 * capture time -- data_core_submit_from() below is the age-aware variant
 * for a node's buffered/back-dated readings.
 *
 * A per-field value capability_encode() can't represent (out of the
 * capability's encodable range -- e.g. MiFlora's uint16 conductivity_us can
 * exceed CAP_SOIL_CONDUCTIVITY's int16 ceiling) is logged (WARN) and that
 * ONE field is skipped, leaving whatever was previously stored for it
 * untouched; every other field in the same frame is still written. This is
 * NOT the same thing as that capability's slot being cleared -- it never
 * is, by an out-of-range reading. */
void data_core_submit_mibeacon(const mibeacon_t *m, const uint8_t via_node[6], int8_t rssi);

/* via_node == NULL means the hub heard this on its own BLE radio; otherwise
 * it is the relaying node's ESP-NOW MAC. rssi is that source's signal
 * strength (0 if unknown/not applicable).
 *
 * age_s back-dates the effective last_seen_s to (now_s - age_s), for
 * readings a node buffered before it could forward them. A reading whose
 * effective time is older than the device's currently stored last_seen_s
 * does not overwrite newer data -- but note this is a "don't regress the
 * live view" guard only. It does NOT insert into an earlier history slot:
 * the sampler writes 15-minute snapshots and the ring files require
 * monotonically increasing (boot_id, rel_s), so out-of-order history
 * inserts are out of scope here. Buffering can only promise "the current
 * reading isn't stale," not "history is backfilled" -- see
 * docs/superpowers/plans/2026-08-02-planthub-m5b-swarm-management.md,
 * "Deferred to M5c and beyond".
 */
void data_core_submit_from(const mibeacon_t *m, const uint8_t via_node[6],
                            int8_t rssi, uint16_t age_s);

void      data_core_snapshot(registry_t *out);

/* Single-device lookup, for a caller that only needs one entry rather than
 * the full ~2KB registry_t (e.g. swarm.c's and webserver/sse.c's own
 * on_sensor_update(), both of which run on the default event-loop task's
 * ~2304 B stack -- see those files). Copies
 * the live device_entry_t (~124 B) for *id into *out under s_mutex, the
 * same locking this file's other accessors use; bounded, allocation-free.
 * Returns false (*out untouched) when the device isn't in the registry. */
bool      data_core_get_device(const device_id_t *id, device_entry_t *out);

/* Forgetting a node must fully forget it: clears via-node attribution (see
 * registry_clear_attribution()) for every device currently attributed to
 * node_mac, under the same mutex every other registry access here uses.
 * Called from the forget HTTP handler's task (api_v1.c), never from the
 * ESP-NOW receive callback -- registry_clear_attribution() itself is a
 * short, bounded, allocation-free scan, same as registry_attribute(), so
 * this follows the exact locking pattern data_core_submit_from() and
 * data_core_snapshot() already use. */
void      data_core_clear_node_attribution(const uint8_t node_mac[6]);

/* A MiFlora battery poll result (battery_poll.c, M6): applies pct to mac's
 * CAP_BATTERY_LEVEL slot (creating the device if this is its first
 * appearance) via registry_set_cap(), NOT registry_attribute() -- a GATT
 * battery read has no frame_cnt of its own, so it must bypass the M5b
 * frame_cnt dedup/arbitration path entirely rather than being wrapped in a
 * synthetic frame and risk being read as a duplicate of frame_cnt 0. A
 * battery poll result is new data by construction -- it was just read live
 * over GATT -- so it is always applied. Posts DATA_EVENT_SENSOR_UPDATE on
 * success, same as data_core_submit_from() does on a merge.
 * Returns false when the registry is full and mac wasn't already present
 * (nothing is created or modified in that case), OR when pct is out of
 * CAP_BATTERY_LEVEL's encodable range (logged WARN, previous value kept if
 * any -- see data_core_submit_mibeacon()'s doc comment for why this never
 * overwrites with a cleared slot; not reachable in practice since pct is a
 * uint8_t 0-100, well inside range, but checked for the same reason every
 * other capability write is). Either way, callers must not advance their
 * own success/backoff state (e.g. a scheduler's last_ok_s) when this
 * returns false, or a reading that was actually dropped would wrongly stop
 * being retried. */
bool      data_core_submit_battery(const uint8_t mac[6], uint8_t pct);
