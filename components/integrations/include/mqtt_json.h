#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capability.h"

/* Capability-driven MQTT/HA payload builders (M2 Task 7, spec Sec.6):
 * topics, state JSON and HA discovery configs are generated from the
 * capability table (capability.h) instead of a hardcoded 5-metric MiFlora
 * shape -- a plant or device can carry any subset of CAPABILITY_COUNT
 * capabilities now, not just V1's fixed five. */

/* Topics. hub is the hub name ("PlantHub-7814"), plant_id is the plant's id
 * (plants_table.h -- 1-255, 0 never valid), dev_id_str is a device's
 * canonical text form (capability.h's device_id_format(), e.g.
 * "ble:A4C138001122"). All return false if the result would not fit. */
bool mqtt_topic_state(char *out, size_t cap, const char *hub, uint8_t plant_id);
/*   -> "planthub/<hub>/plant/<id>/state"                                   */
bool mqtt_topic_device_state(char *out, size_t cap, const char *hub, const char *dev_id_str);
/*   -> "planthub/<hub>/device/<id-string>/state"                          */
bool mqtt_topic_avail(char *out, size_t cap, const char *hub);
/*   -> "planthub/<hub>/status"                                             */
bool mqtt_topic_discovery(char *out, size_t cap, uint8_t plant_id, uint8_t cap_id);
/*   -> "homeassistant/sensor/planthub_plant_<id>_<metric>/config", where
 *      <metric> is the capability's name (capability_get()->name) with
 *      every '.' turned into '_' (e.g. "soil.moisture" -> "soil_moisture").
 *      Returns false for an unknown cap_id too. */
bool mqtt_topic_device_discovery(char *out, size_t cap, const char *dev_id_str, uint8_t cap_id);
/*   -> "homeassistant/sensor/planthub_device_<id-string>_<metric>/config"
 *      (spec Sec.6, amended -- see mqtt_json_device_discovery() below).
 *      Returns false for an unknown cap_id too. */

/* The retained-cleanup payload (spec Sec.6: "publishes an empty retained
 * payload" on plant delete / capability unbind / device removal): always
 * the empty string. A named constant so every cleanup call site
 * (mqtt_pub.c) says what it means rather than a bare "", and so host tests
 * have something concrete to assert against -- the RETAINED part of
 * "empty retained payload" is an esp_mqtt_client_publish() flag, set at
 * every one of those call sites, not something this string-only layer can
 * express or test. */
#define MQTT_CLEANUP_PAYLOAD ""

/* State payload: one JSON field per present[cap_id], keyed by the
 * capability's full name (capability_get(cap_id)->name, e.g.
 * "soil.moisture") and formatted to that capability's display precision
 * (capability_get(cap_id)->precision decimal places). Absent capabilities
 * are omitted entirely, never emitted as null:
 *   {"soil.moisture":42,"air.temperature":21.3}
 * value[i] is the DECODED (real-unit) value, e.g. from
 * plants_cap_value()/capability_decode() -- never a raw int16 storage
 * value. Returns false only if it would not fit; an all-absent st still
 * succeeds, producing "{}" (mirrors the old mqtt_json_state()'s "rssi
 * always emitted" case going away -- every capability, rssi included, is
 * now just another optionally-bound field). */
typedef struct {
    bool  present[CAPABILITY_COUNT];
    float value[CAPABILITY_COUNT];
} mqtt_state_t;
bool mqtt_json_state(char *out, size_t cap, const mqtt_state_t *st);

/* One HA discovery config for plant_id's cap_id (spec Sec.6). device_class,
 * unit_of_measurement and suggested_display_precision come straight from
 * the capability table (capability.h), EXCEPT unit_of_measurement: HA
 * expects its own spelling for a couple of units the table stores in the
 * REST/bytecode form ("C" -> "°C", "lux" -> "lx") -- see this file's
 * .c for ha_unit_of_measurement(). Every other unit (%, uS/cm, hPa, dBm)
 * passes through unchanged; the capability table itself is never touched
 * for this (M1 bytecode and the REST contract depend on its spellings).
 *
 * plant_name "" falls back to "Plant <id>" (built inside this function).
 * The value_template addresses the field with bracket syntax
 * ({{ value_json['soil.moisture'] }}), not dotted attribute access --
 * required because the capability name itself contains '.'.
 * Entity id: planthub_plant_<id>_<metric>, metric = capability name with
 * '.' -> '_' (mqtt_topic_discovery()'s scheme, reused here for uniq_id).
 * Returns false for an unknown cap_id or if it would not fit. */
bool mqtt_json_discovery(char *out, size_t cap, const char *hub, uint8_t plant_id,
                          const char *plant_name, uint8_t cap_id);

/* Device-form discovery config for dev_id_str's cap_id -- spec Sec.6,
 * amended during M2 implementation (see the spec's own note): a device
 * capability gets a discovery entity ONLY when that (device id, cap id)
 * pair is not already exposed through a plant binding -- the literal
 * original wording ("one entity per bound plant-capability and per device
 * capability") would have surfaced a bound probe twice in HA, once as its
 * plant and once as itself. mqtt_pub.c owns the dedup DECISION (it has to
 * consult the plants table); this function just builds the payload once
 * the caller has already decided to. Same shape as mqtt_json_discovery()
 * above (device_class/unit_of_measurement/suggested_display_precision from
 * the table, bracket-syntax value_template, HA unit translation) except:
 *   - uniq_id/dev.ids: planthub_device_<id-string>_<metric> /
 *     planthub_device_<id-string> (no numeric id to fall back to)
 *   - stat_t: the device state topic (mqtt_topic_device_state()'s shape)
 *   - display_name "" falls back to dev_id_str itself, not "Plant <id>"
 * When a probe's capability later gets bound to a plant, ITS device-form
 * entity becomes stale and must be cleared via the retained-cleanup path
 * (mqtt_pub.c's mqtt_pub_device_cap_bound()) -- this function has no part
 * in that; it only ever builds a fresh, currently-valid config.
 * Returns false for an unknown cap_id or if it would not fit. */
bool mqtt_json_device_discovery(char *out, size_t cap, const char *hub, const char *dev_id_str,
                                const char *display_name, uint8_t cap_id);
