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
