/* swarm_ingest.h -- the transport-neutral ingest of the four Zigbee device
 * frames (announce / device-gone / measurement / tuya-dp) into data_core +
 * the actor table + the dev_profiles mapping engine.
 *
 * Originally lifted from the (since retired, 2026-10) wireless Zigbee bridge
 * node's hub-side task; today its one caller is h2_link.c, which feeds it the
 * frames the two-chip hub's wired H2 coordinator sends over the UART.
 * `via_mac` is the forwarding node's MAC, or NULL for the hub's own (local)
 * radio -- the H2 -- in which case no data_core_set_via() node attribution is
 * recorded. These functions take data_core's / the actor table's own locks;
 * callers need hold no lock. */
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
