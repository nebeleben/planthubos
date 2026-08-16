#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "capability.h"

/* Registry v2 (M2 "Device Model 2.0"): one slot per physical device
 * (BLE/ESP-NOW/Zigbee, keyed by device_id_t -- capability.h), each carrying
 * up to CAPABILITY_COUNT independent capability readings instead of a
 * single hardcoded MiFlora-shaped mibeacon_t. See
 * .superpowers/sdd/2026-08-16-planthub-v2-m2-device-model/ for the design.
 *
 * V1's sensor_entry_t/mibeacon_t-shaped registry (mac-keyed, one fixed set
 * of MiFlora fields) is gone from this file; consumers not yet rewired onto
 * capability ids (Tasks 3-7) read a temporary compatibility shim instead --
 * see registry_compat.h, every symbol there tagged M2-SHIM. */

#define REGISTRY_MAX_DEVICES 16

typedef struct { int16_t raw; uint32_t updated_s; bool valid; } cap_slot_t;

typedef struct {
    bool        in_use;
    device_id_t id;
    uint32_t    last_seen_s;
    cap_slot_t  caps[CAPABILITY_COUNT];
    /* attribution (M5b rules, carried over verbatim) */
    bool     via_node_valid;
    uint8_t  via_node[6];
    int8_t   best_rssi;
    uint32_t attributed_s;
    /* M2 internal bookkeeping ONLY -- not part of the documented
     * per-capability model, no consumer should read this directly. Mirrors
     * mibeacon_t.frame_cnt's uint8_t width (the only producer of frame_cnt
     * today); exists purely so registry_attribute() can reproduce M5b's
     * exact "a new frame_cnt always wins, a duplicate frame_cnt arbitrates
     * on rssi" rule without a second, separately-keyed lookup table. This is
     * the one field in this struct not given verbatim by the task brief --
     * see task-2-report.md's Deviations section. */
    uint8_t  last_frame_cnt;
} device_entry_t;

typedef struct { device_entry_t devices[REGISTRY_MAX_DEVICES]; } registry_t;

void registry_init(registry_t *r);

/* -1 = absent. Matches on device_id_t equality (kind AND addr) -- a BLE and
 * an ESP-NOW device that happen to share the same 6-byte MAC are distinct
 * entries. */
int  registry_find(const registry_t *r, const device_id_t *id);
int  registry_count(const registry_t *r);

/* Writes one capability slot. Finds or creates the device (returns -1 when
 * the table is full and the device is unknown); raw == CAP_VALUE_NONE
 * clears the slot (valid=false) without deleting the device. Always
 * refreshes last_seen_s, whether this call sets or clears a value -- same
 * "any accepted write is presence" contract V1's registry_update_from() and
 * registry_set_battery() both had. Does not touch attribution: pair with
 * registry_attribute() when M5b's frame-level arbitration applies (the
 * normal MiBeacon ingest path, data_core.c), or call on its own when there
 * is no such contest (e.g. a GATT battery poll, still bypassing frame_cnt
 * dedup entirely -- exactly like V1's registry_set_battery()). Returns the
 * device index on success. */
int  registry_set_cap(registry_t *r, const device_id_t *id, uint8_t cap_id,
                      int16_t raw, uint32_t now_s);

/* Attribution decision, unchanged semantics from M5b (see the original
 * registry_update_from() comment this carries forward verbatim):
 *   - A brand-new device (never seen before) is unconditionally attributed
 *     to this reporter.
 *   - On a NEW frame_cnt (differs from the one last seen for this device),
 *     the reporter of that frame unconditionally becomes the attributed
 *     source.
 *   - On a DUPLICATE frame_cnt from a different reporter, the strongest
 *     rssi wins and updates via_node/best_rssi.
 *   - A direct BLE reception (via_node == NULL) always outranks any
 *     node-relayed reading, win or lose on rssi -- the hub hearing a sensor
 *     itself is strictly better than a relayed copy.
 * Returns true when this reporter owns the device now (attribution was
 * set/kept to this reporter); false when it lost the arbitration, or when
 * the table is full and the device is unknown (nothing is created or
 * modified in that case). Finds or creates the device like
 * registry_set_cap() above; always refreshes last_seen_s for an
 * existing/created device regardless of the arbitration outcome -- "seen"
 * and "attributed" are different questions, same as V1. */
bool registry_attribute(registry_t *r, const device_id_t *id, uint32_t frame_cnt,
                        const uint8_t via_node[6], int8_t rssi, uint32_t now_s);

/* Forgetting a node (see swarm_store_forget_node()) must forget it fully:
 * without this, every device entry currently attributed to node_mac keeps
 * reporting it as the "via" source forever, since is_paired_node()
 * (swarm.c) rejects that MAC's frames from ever reaching
 * registry_attribute() again -- nothing else could ever re-attribute it.
 * Clears via_node_valid (and zeroes via_node/best_rssi) for every entry
 * currently attributed to node_mac; entries attributed to a direct hub
 * reception or to a different node are untouched. attributed_s is left as
 * the value the entry had before this cleared -- it records when
 * attribution last CHANGED, and clearing to "no attribution" isn't a new
 * source claiming the device, just this one being taken away. */
void registry_clear_attribution(registry_t *r, const uint8_t node_mac[6]);
