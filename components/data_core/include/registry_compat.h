#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "registry.h"
#include "mibeacon.h"

/* M2-SHIM: bridges the V1 sensor_entry_t/mibeacon_t-shaped registry read
 * API to the V2 per-capability registry (registry.h), per RULING-1 of the
 * M2 device-model plan: ~9 consumers (webserver/sensors_json.[ch],
 * webserver/sse.c, webserver/api_v1.c, integrations/mqtt_pub.c,
 * integrations/influx.c, sampler/sampler.c, plants/plants.[ch],
 * rules/rules_resolver.c, swarm/swarm.c's on_sensor_update()) still read
 * the registry in the old shape and are rewired onto capability ids by
 * Tasks 3-7, not this one. Every symbol here is BLE/MiFlora-only (the only
 * device kind that existed pre-M2) and read-only -- nothing here ever
 * writes the live registry.
 *
 * DELETE this file, registry_compat.c, and every M2-SHIM-tagged call site
 * once Tasks 3-7 have rewired all of the above onto registry.h/capability.h
 * directly. */

#define REGISTRY_MAX_SENSORS REGISTRY_MAX_DEVICES   /* M2-SHIM */

typedef struct {
    bool       in_use;
    uint8_t    mac[6];
    uint32_t   last_seen_s;
    mibeacon_t latest;      /* decoded back from the device's capability slots */

    /* Attribution (M5b), carried over unchanged from the live device_entry_t. */
    bool      via_node_valid;
    uint8_t   via_node[6];
    int8_t    best_rssi;
    uint32_t  attributed_s;
} sensor_entry_t;   /* M2-SHIM: V1 shape */

typedef struct { sensor_entry_t sensors[REGISTRY_MAX_SENSORS]; } legacy_registry_t;   /* M2-SHIM */

/* M2-SHIM: V1-shaped snapshot of the live registry's BLE-kind devices,
 * decoded through capability_decode() back into mibeacon_t's native units
 * (deci-C, %, lux, uS/cm, %). Devices of any other device_kind_t are
 * skipped -- they have no V1 representation and none exist yet. */
void data_core_snapshot_legacy(legacy_registry_t *out);

/* M2-SHIM: V1's mac-keyed registry_find(), over a legacy_registry_t snapshot. */
int legacy_registry_find(const legacy_registry_t *r, const uint8_t mac[6]);
