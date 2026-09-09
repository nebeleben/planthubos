#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "swarm_frame.h"

/* Hub-side bridge table (M7 Task 7): one entry per zigbee-role node this
 * hub has ever heard from, each carrying that node's last-known zigbee
 * coordinator status plus the set of end devices it has announced. This is
 * the hub's OWN bookkeeping, distinct from (and does not replace) the
 * per-node ESP-NOW RAM stats swarm.c's s_stats[] already keeps, and
 * distinct from data_core's registry -- a device lives in BOTH: here,
 * keyed by (bridge node mac, dev kind+addr), so the hub knows which bridge
 * to ask/resync; in the registry, keyed by device_id_t, so it has capability
 * readings and shows up in the UI/API the same as any BLE device.
 *
 * Pure C, no ESP-IDF: swarm.c owns the tmp+rename persisted write (same
 * discipline as zigbee.c's zb_store.c) and every FreeRTOS/mutex concern.
 * This file only manipulates the in-memory table -- see
 * tests/host/test_bridge_table.c. bridge_table_deserialize()'s internal
 * scratch table is a file-static (too big for a stack frame) rather than a
 * FreeRTOS/ESP-IDF primitive, so the pure-C contract still holds; callers
 * must serialise access the same way swarm.c already does via
 * s_bridge_io_mutex.
 *
 * BRIDGE_MAX_NODES intentionally duplicates SWARM_MAX_NODES's value (6)
 * rather than including swarm_store.h to get it: swarm_store.h pulls in
 * esp_err.h, which would break this header's pure-C, host-testable
 * contract (the same reason swarm_frame.h itself carries no ESP-IDF
 * dependency). swarm.c _Static_asserts the two never silently drift apart. */
#define BRIDGE_MAX_NODES 6
#define BRIDGE_MAX_DEVICES 16

typedef struct {
    uint8_t  mac[6];
    bool     in_use;
    swarm_coord_status_t status;
    bool     status_valid;
    bool     synced_once;   /* RAM only -- never persisted, see bridge_table_serialize() */
    uint8_t  count;
    swarm_device_announce_t dev[BRIDGE_MAX_DEVICES];
} bridge_node_t;

typedef struct {
    bridge_node_t n[BRIDGE_MAX_NODES];
} bridge_table_t;

void bridge_table_init(bridge_table_t *t);

/* Finds mac's node entry. create=false: pure lookup, NULL if absent.
 * create=true: also claims the first free slot for an absent mac -- NULL
 * only when every slot is already in use (BRIDGE_MAX_NODES exceeded); a
 * freshly-claimed slot is zero-initialised (in_use=true, mac set, no
 * status yet, count 0). */
bridge_node_t *bridge_table_node(bridge_table_t *t, const uint8_t mac[6], bool create);

/* Inserts a's device under mac's node, or updates it in place if mac
 * already announced that (kind, addr) before -- matched via
 * bridge_table_find_device()'s own kind+addr comparison, scoped to this
 * node. Creates mac's node entry if it doesn't exist yet (see
 * bridge_table_node() above). Returns false when the node table is full
 * (no slot for a new mac) or -- the more common case -- this node's device
 * table is already full (BRIDGE_MAX_DEVICES) and a is not one of its
 * existing devices; nothing is modified either way. */
bool bridge_table_upsert(bridge_table_t *t, const uint8_t mac[6], const swarm_device_announce_t *a);

/* Removes dev from mac's node. Returns false (nothing modified) if mac has
 * no node entry, or dev isn't among its devices. */
bool bridge_table_remove(bridge_table_t *t, const uint8_t mac[6], const swarm_dev_addr_t *dev);

/* Wipes mac's entire node entry (status and every device it announced) --
 * called when the node itself is forgotten (swarm.c's swarm_forget_node_stats()),
 * not when one of its devices leaves the zigbee network (that's
 * bridge_table_remove(), driven by a DEVICE_GONE frame). A no-op if mac has
 * no entry. */
void bridge_table_forget_node(bridge_table_t *t, const uint8_t mac[6]);

/* Which node (if any) currently has dev among its announced devices --
 * matched on kind+addr, across every in-use node. NULL if no node has it. */
const bridge_node_t *bridge_table_find_device(const bridge_table_t *t, const swarm_dev_addr_t *dev);

/* Writes the whole table's image (magic+version, every in-use node's mac/
 * status/device count, then each device via swarm_encode_device_announce(),
 * length-prefixed) into buf. Returns the image length, or 0 if buf is too
 * small or a device somehow fails to encode -- see bridge_table.c. */
size_t bridge_table_serialize(const bridge_table_t *t, uint8_t *buf, size_t cap);

/* Loads an image written by bridge_table_serialize(). Returns false --
 * leaving *t untouched -- on a bad magic/version, an out-of-range node or
 * device count, a device that fails to decode, or a buffer with trailing or
 * missing bytes (len must account for the image exactly, same discipline
 * as zb_store_deserialize()). A partly-loaded table is worse than none: it
 * would silently drop devices/nodes the hub actually still knows about. */
bool bridge_table_deserialize(bridge_table_t *t, const uint8_t *buf, size_t len);
