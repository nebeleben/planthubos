#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include "registry.h"
#include <stdbool.h>
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

/* Forgetting a node must fully forget it: clears via-node attribution (see
 * registry_clear_attribution()) for every sensor currently attributed to
 * node_mac, under the same mutex every other registry access here uses.
 * Called from the forget HTTP handler's task (api_v1.c), never from the
 * ESP-NOW receive callback -- registry_clear_attribution() itself is a
 * short, bounded, allocation-free scan, same as registry_update_from(), so
 * this follows the exact locking pattern data_core_submit_from() and
 * data_core_snapshot() already use. */
void      data_core_clear_node_attribution(const uint8_t node_mac[6]);

/* A MiFlora battery poll result (battery_poll.c, M6): applies pct to mac's
 * registry entry (creating it if this is the sensor's first appearance)
 * via registry_set_battery(), NOT registry_update_from() -- see that
 * function's doc comment for why a battery read must bypass the frame_cnt
 * dedup path entirely rather than being wrapped in a synthetic mibeacon_t
 * and passed through data_core_submit_from(). Posts DATA_EVENT_SENSOR_UPDATE
 * on success, same as data_core_submit_from() does on a merge.
 * Returns false only when the registry is full and mac wasn't already
 * present -- callers must not advance their own success/backoff state
 * (e.g. a scheduler's last_ok_s) when this returns false, or a reading
 * that was actually dropped would wrongly stop being retried. */
bool      data_core_submit_battery(const uint8_t mac[6], uint8_t pct);
