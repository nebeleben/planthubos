/* swarm_ingest.h -- the transport-neutral ingest of the four Zigbee bridge
 * frames (announce / device-gone / measurement / tuya-dp) into data_core +
 * the actor table + the dev_profiles mapping engine.
 *
 * Lifted VERBATIM from swarm.c's bridge_task switch so the ESP-NOW path and
 * the wired-UART (H2) path cannot diverge: whatever the hub does with a frame
 * arriving over ESP-NOW from a wireless bridge, it does identically with the
 * same frame arriving over the UART from the local H2 coordinator. The only
 * difference is attribution: `via_mac` is the forwarding node's MAC for the
 * ESP-NOW path, or NULL for the LOCAL radio (the H2) -- NULL means "this hub's
 * own coordinator", so no data_core_set_via() node attribution is recorded.
 *
 * Pure of transport and of swarm's own bridge-table/persist state: the
 * bridge_table upsert/remove and mark_bridge_dirty() stay in swarm.c (they are
 * ESP-NOW routing/persistence state the local radio has no equivalent of).
 * These functions take data_core's / the actor table's own locks as the
 * original bodies did; callers may hold swarm's s_bridges_mutex across them
 * (the ESP-NOW path does) or no lock at all (the UART path). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "swarm_frame.h"

/* A newly interviewed / re-announced device: upserts the registry row,
 * mirrors manufacturer/model, (re)declares its actions, and -- when via_mac
 * is non-NULL -- records the forwarding node as its attribution. */
void swarm_ingest_announce(const swarm_device_announce_t *a, const uint8_t *via_mac, uint32_t now_s);

/* A device left the network: clears its stale via-node attribution (the
 * registry itself never deletes rows). */
void swarm_ingest_device_gone(const swarm_device_gone_t *g);

/* One capability reading. Returns false ONLY when the device is unknown to
 * the registry (the caller should request a resync/replay); true otherwise,
 * including an intentional dev_profiles suppress-drop and a successful submit.
 * extra_age_s is time the frame spent queued after receipt (0 for a freshly
 * decoded UART frame); it is added to the frame's own age_s. */
bool swarm_ingest_measurement(const swarm_measurement_t *m, uint32_t extra_age_s, uint32_t now_s);

/* One raw Tuya EF00 datapoint: always recorded for discovery, and submitted
 * as a mapped capability when this device's dev_profiles DP-map covers it. */
void swarm_ingest_tuya_dp(const swarm_tuya_dp_t *d, uint32_t now_s);
