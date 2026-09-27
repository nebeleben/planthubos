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
 * of MiFlora fields) is gone from this file; every consumer reads
 * capability ids directly now (Tasks 2-7 rewired the last holdouts --
 * see task-7-report.md, "RULING-1" -- the temporary registry_compat.h/
 * storage_compat.h compatibility shims Tasks 2-4 introduced are deleted). */

#define REGISTRY_MAX_DEVICES 16
#define REGISTRY_EXTRA_EP_CAPS 4

typedef struct { int16_t raw; uint32_t updated_s; bool valid; } cap_slot_t;

typedef struct {
    bool        in_use;
    device_id_t id;
    /* M2 internal bookkeeping ONLY -- not part of the documented
     * per-capability model, no consumer should read this directly. Mirrors
     * mibeacon_t.frame_cnt's uint8_t width (the only producer of frame_cnt
     * today); exists purely so registry_attribute() can reproduce M5b's
     * exact "a new frame_cnt always wins, a duplicate frame_cnt arbitrates
     * on rssi" rule without a second, separately-keyed lookup table. This is
     * the one field in this struct not given verbatim by the task brief --
     * see task-2-report.md's Deviations section. Placed HERE deliberately
     * (between `id`, which ends at offset 10, and `last_seen_s`, which needs
     * 4-byte alignment and so starts at offset 12 regardless): this is
     * already-dead alignment padding on every ABI this project targets, so
     * the field is free -- appending it after `attributed_s` instead grew
     * the struct's TRAILING padding by a real 4 bytes/device (124B->128B,
     * registry_t 1984B->2048B for REGISTRY_MAX_DEVICES=16), caught in code
     * review. The brief's eight named fields below keep their exact
     * relative order. */
    uint8_t  last_frame_cnt;
    uint32_t    last_seen_s;
    cap_slot_t  caps[CAPABILITY_COUNT];
    /* Multi-endpoint (lazy side-list): cap_endpoint[c] is the DEFAULT (lowest)
     * endpoint whose value lives in caps[c]; 0 == untracked/legacy. Additional
     * (cap,endpoint) instances live in extra_ep_caps. */
    uint8_t     cap_endpoint[CAPABILITY_COUNT];
    struct { uint8_t cap_id; uint8_t endpoint; cap_slot_t slot; } extra_ep_caps[REGISTRY_EXTRA_EP_CAPS];
    uint8_t     extra_ep_cap_count;
    /* attribution (M5b rules, carried over verbatim) */
    bool     via_node_valid;
    uint8_t  via_node[6];
    int8_t   best_rssi;
    uint32_t attributed_s;
    /* Transient (never serialized): true for a row restored from the boot
     * snapshot (registry_persist.h) that has not yet been heard from live
     * this boot. last_seen_s/updated_s are uptime seconds and reset across a
     * reboot, so a restored row's age is taken from the snapshot epoch and
     * marked stale by devices_json instead of reading as just-seen. Cleared
     * the moment a live value arrives for the device. */
    bool     snapshot_only;
    /* Device identity (device-mapping-profiles Task 7): captured from a
     * Zigbee announce's manufacturer/model strings (swarm.c's
     * SWARM_MSG_DEVICE_ANNOUNCE case, via data_core_set_identity()), so
     * /api/v1/devices can surface them and dev_profiles matching (a later
     * task) has something to key a profile lookup on. Plain in-RAM mirror
     * only -- empty ("") until an announce carrying them arrives, and never
     * itself persisted here: registry_persist.c serializes fields
     * explicitly and does not include these, and zb_store (Task 2) already
     * owns durable manufacturer/model storage for zigbee bridge devices. */
    char     manufacturer[32];
    char     model[32];
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

/* Multi-endpoint variant: writes the (cap_id, endpoint) pair explicitly,
 * instead of always targeting the default/main slot the way
 * registry_set_cap() does. registry_set_cap() is a thin wrapper over this
 * that always passes the device's current default endpoint (or claims
 * `endpoint` as the default on a device's first write for that cap_id), so
 * every existing single-endpoint caller is unaffected.
 *
 * The FIRST endpoint ever seen for a (device, cap_id) becomes that cap's
 * tracked default and lives in caps[cap_id] (see device_entry_t's
 * cap_endpoint[] doc comment). A write at that same default endpoint keeps
 * updating caps[cap_id]. A write at a DIFFERENT, higher endpoint lands in
 * (or updates its existing row in) extra_ep_caps -- dropped silently once
 * REGISTRY_EXTRA_EP_CAPS rows are already in use (a diagnostic-only budget:
 * every value should still be requestable via zigbee re-poll on demand,
 * per the design's endpoint-mapping-is-lossy note). A write at a LOWER
 * endpoint than the current default promotes it: the new endpoint becomes
 * the default (caps[cap_id]), and the previous default's value is demoted
 * into extra_ep_caps -- interview order is lowest-endpoint-first, so this
 * only matters for the rare device that reports its lowest endpoint late.
 * Returns the device index (find-or-create semantics identical to
 * registry_set_cap()), or -1 when cap_id is out of range or the table is
 * full and the device is unknown. */
int  registry_set_cap_ep(registry_t *r, const device_id_t *id, uint8_t cap_id,
                         uint8_t endpoint, int16_t raw, uint32_t now_s);

/* Reads the (cap_id, endpoint) slot registry_set_cap_ep() wrote. endpoint ==
 * 0, or equal to the cap's tracked default endpoint, returns the main
 * caps[cap_id] slot (so a caller that doesn't care about endpoints at all
 * can keep passing 0). Any other endpoint is looked up in extra_ep_caps.
 * Returns NULL when the device is unknown, cap_id is out of range, or the
 * device never reported that specific endpoint for that cap. Read-only:
 * unlike the two set_cap* entry points, never creates a device or mutates
 * anything. */
const cap_slot_t *registry_get_cap_ep(const registry_t *r, const device_id_t *id,
                                      uint8_t cap_id, uint8_t endpoint);

/* Finds id's registry slot, or claims a free one when id is unknown -- same
 * find-or-create primitive registry_set_cap()/registry_attribute() already
 * use internally, exposed directly for a caller that has confirmed a device
 * is present but has no capability value (or attribution decision) to write
 * yet (M5a gate fix 3: a connect-only GATT device's matched advertisement,
 * before any GATT read has happened -- see ble_collector.c's decode_adv_item()
 * and data_core_find_or_create_index(), which wraps this under s_mutex).
 *
 * now_s stamps the NEW entry's last_seen_s -- ONLY on the create path, since
 * this call is itself the sighting that justifies the slot existing at all.
 * An ALREADY-known device's last_seen_s is left exactly as it was: finding
 * it here asserts nothing new about when it was last heard from, unlike
 * registry_set_cap()/registry_attribute(), which always refresh it because
 * they represent a real reading. Calling this repeatedly for the same known
 * device is therefore side-effect-free after the first call.
 *
 * Returns -1 when the table is full and id is unknown -- nothing is created
 * or modified in that case, same as every other find_or_create()-backed
 * entry point here. The caller decides what "full" means for it; this
 * function only guarantees it never silently overwrites another device's
 * slot to make room. */
int registry_find_or_create(registry_t *r, const device_id_t *id, uint32_t now_s);

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

/* M7 Task 7: attributes an ALREADY-RESOLVED device (idx, from
 * registry_find_or_create()/registry_find()) to node_mac -- a zigbee
 * bridge's DEVICE_ANNOUNCE names its own reporting node directly and has no
 * frame_cnt/rssi of its own to arbitrate with, unlike registry_attribute()'s
 * MiBeacon contest above. Reuses set_attribution() with rssi 0 (meaningless
 * here; a zigbee device never runs through registry_attribute()'s rssi
 * comparison, so this never gets contested) and the entry's own
 * last_seen_s as its timestamp. A no-op if idx is out of range or the slot
 * isn't in_use. */
void registry_set_via(registry_t *r, int idx, const uint8_t node_mac[6]);

/* M7 Task 7 fix round 1 (critical #2): a zigbee bridge's DEVICE_GONE means
 * ITS device left, not that the whole reporting node is gone (that's
 * registry_clear_attribution() above, driven by a FORGET) -- but the
 * registry has no delete (see this header's own top comment), so the
 * device entry stays; only its via-node attribution is stale once the
 * device is no longer behind that bridge. Resets via_node_valid/via_node/
 * best_rssi for idx alone, same fields registry_clear_attribution() resets
 * per-entry, just scoped to one already-resolved index instead of a scan
 * over every entry attributed to a mac. A later re-ANNOUNCE (of this
 * device rejoining, here or on a different bridge) re-attributes via
 * registry_set_via() above. A no-op if idx is out of range or not in_use. */
void registry_clear_via(registry_t *r, int idx);
