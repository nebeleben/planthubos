#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mibeacon.h"

#define REGISTRY_MAX_SENSORS 16

typedef struct {
    bool      in_use;
    uint8_t   mac[6];
    uint32_t  last_seen_s;
    mibeacon_t latest;    /* merged latest values; has_* flags accumulate */

    /* Attribution (M5b): which radio currently "owns" this sensor and how
     * strongly it hears it -- see registry_update_from() for the rule. */
    bool      via_node_valid;  /* false: the hub heard it on its own BLE radio */
    uint8_t   via_node[6];     /* valid only when via_node_valid */
    int8_t    best_rssi;       /* rssi of the current attributed source */
    uint32_t  attributed_s;    /* now_s when the attributed source last changed */
} sensor_entry_t;

typedef struct {
    sensor_entry_t sensors[REGISTRY_MAX_SENSORS];
} registry_t;

void registry_init(registry_t *r);

/* 1 merged, 0 dup, -1 full.
 *
 * via_node == NULL means the hub heard this frame on its own BLE radio;
 * otherwise it is the ESP-NOW MAC of the node that relayed it, and rssi is
 * that node's reading of the sensor's signal.
 *
 * Attribution rule (what makes "move a plant, the nearest node takes over"
 * work with no election protocol):
 *   - On a NEW frame (frame_cnt differs from the stored one), the reporter
 *     of that frame unconditionally becomes the attributed source.
 *   - On a DUPLICATE frame (same frame_cnt) from a different reporter, the
 *     strongest rssi wins and updates via_node/best_rssi.
 *   - A direct BLE reception (via_node == NULL) always outranks any
 *     node-relayed reading, win or lose on rssi -- the hub hearing a sensor
 *     itself is strictly better than a relayed copy.
 */
int  registry_update_from(registry_t *r, const mibeacon_t *m, uint32_t now_s,
                           const uint8_t via_node[6], int8_t rssi);

/* Wrapper kept for existing callers/tests: equivalent to a direct hub
 * reception with no known rssi. */
int  registry_update(registry_t *r, const mibeacon_t *m, uint32_t now_s);

int  registry_find(const registry_t *r, const uint8_t mac[6]);
int  registry_count(const registry_t *r);

/* Applies a battery-poll result (battery_poll.c, M6): finds/creates the
 * entry for mac, sets has_battery/battery_pct, and refreshes last_seen_s.
 * Deliberately bypasses registry_update_from()'s frame_cnt dedup entirely
 * (it neither reads nor writes frame_cnt/product_id/any other field) --
 * unlike a MiBeacon advertisement, a GATT battery read has no frame_cnt of
 * its own, and going through registry_update_from() with a synthetic
 * mibeacon_t{.frame_cnt=0} would get silently treated as a duplicate
 * whenever the stored entry's frame_cnt already happens to be 0, dropping
 * a freshly-read value. A battery poll result is new data by construction
 * -- it was just read live over GATT -- so it is always applied.
 * Returns 1 on success, -1 if the registry is full and mac was not already
 * present (nothing is created or modified in that case). */
int  registry_set_battery(registry_t *r, const uint8_t mac[6], uint8_t pct, uint32_t now_s);

/* Forgetting a node (see swarm_store_forget_node()) must forget it fully:
 * without this, every sensor entry currently attributed to node_mac keeps
 * reporting it as the "via" source forever, since is_paired_node()
 * (swarm.c) rejects that MAC's frames from ever reaching
 * registry_update_from() again -- nothing else could ever re-attribute it.
 * Clears via_node_valid (and zeroes via_node/best_rssi) for every entry
 * currently attributed to node_mac; entries attributed to a direct hub
 * reception or to a different node are untouched. attributed_s is left as
 * the value the entry had before this cleared -- it records when
 * attribution last CHANGED, and clearing to "no attribution" isn't a
 * new source claiming the sensor, just this one being taken away.
 * Returns the number of entries cleared. */
int  registry_clear_attribution(registry_t *r, const uint8_t node_mac[6]);
