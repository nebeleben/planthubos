#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include "registry.h"
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(PLANTHUB_DATA_EVENT);
enum { DATA_EVENT_SENSOR_UPDATE };

/* A relayed reading older than this is dropped outright rather than
 * resurrected as "current": a node that buffered through a long outage
 * should not make a stale value look live. */
#define DATA_CORE_MAX_AGE_S 1800

esp_err_t data_core_init(void);

/* via_node == NULL means the hub heard this on its own BLE radio; otherwise
 * it is the relaying node's ESP-NOW MAC. rssi is that source's signal
 * strength (0 if unknown/not applicable).
 *
 * age_s back-dates the effective last_seen_s to (now_s - age_s), for
 * readings a node buffered before it could forward them. A reading whose
 * effective time is older than the sensor's currently stored last_seen_s
 * does not overwrite newer data -- but note this is a "don't regress the
 * live view" guard only. It does NOT insert into an earlier history slot:
 * the sampler writes 15-minute snapshots and the ring files require
 * monotonically increasing (boot_id, rel_s), so out-of-order history
 * inserts are out of scope here. Buffering can only promise "the current
 * reading isn't stale," not "history is backfilled" -- see
 * docs/superpowers/plans/2026-08-02-planthub-m5b-swarm-management.md,
 * "Deferred to M5c and beyond".
 */
void data_core_submit_from(const mibeacon_t *m, const uint8_t via_node[6],
                            int8_t rssi, uint16_t age_s);

/* Wrapper kept for existing callers: a direct hub reception, no known rssi,
 * age_s = 0 (just heard). */
void      data_core_submit(const mibeacon_t *m);
void      data_core_snapshot(registry_t *out);
