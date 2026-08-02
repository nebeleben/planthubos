#pragma once
#include "cJSON.h"
#include "registry.h"

/* Builds the JSON object for one sensor, as served by GET /api/v1/sensors
 * and streamed over SSE.
 *
 * "via" (M5b, attribution): null when the hub heard this sensor on its own
 * BLE radio directly; otherwise {"mac":"AA:BB:...","name":<node's stored
 * name, or null if unset>,"rssi":<best_rssi>} -- the node that
 * registry_update_from()'s "strongest rssi wins" rule currently attributes
 * this sensor's data to (see registry.h). */
cJSON *sensor_json(const sensor_entry_t *e);
