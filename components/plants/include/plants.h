#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "plants_table.h"
#include "registry.h"

/* NVS-backed plant registry (M8 Task 2): a single blob keyed "plants" in
 * namespace "planthub" (shared with swarm_store.c -- see swarm_store.c's
 * locking-invariant comment for the two-phase RAM-mutex/NVS-mutex persist
 * discipline this mirrors byte for byte, just against a different key and
 * blob). RAM cache + mutex layered on top of plants_table.h's pure logic;
 * every mutating call below persists to flash before returning.
 *
 * The on-disk blob is an explicit packed mirror struct (format byte 1 +
 * next_id + 16 x {id, in_use:u8, mac[6], mac_valid:u8, name[33]}), never a
 * raw dump of plants_table_t -- the host struct's bool fields and padding
 * are not a stable on-disk shape across compilers/targets. A wrong length
 * or an unrecognised format byte is loudly logged and treated as "start
 * empty" -- this never fails boot. */

/* Init: loads the NVS blob (missing -> empty table), then runs the one-boot
 * migration (Task 4; a stub no-op until then). Call from main.c after
 * data_core_init() and the littlefs mount, HUB ROLE ONLY (nodes keep no
 * plants). storage_base is "/storage", or NULL when storage/littlefs is
 * unavailable -- plants_delete()'s ring-file cleanup then has nothing to
 * remove and is skipped rather than failing. Never fails boot on its own
 * account (any NVS read problem is log-and-continue, defaults to an empty
 * table); the only error this can genuinely return is mutex allocation
 * (ESP_ERR_NO_MEM), which main.c must also log-and-continue past, same as
 * every other storage-adjacent init call there -- ESP_ERROR_CHECK is not
 * appropriate here. */
esp_err_t plants_init(const char *storage_base /* "/storage", or NULL when storage unavailable */);

/* mac -> plant id, auto-creating (and persisting) on first sight.
 * TASK CONTEXT ONLY (may write NVS). 0 = table full (logged once/mac/boot). */
uint8_t plants_resolve_or_create(const uint8_t mac[6]);

/* Read-only snapshot for the API/MQTT/UI. Safe to call before plants_init()
 * has run (e.g. a NODE that fell back to its setup portal, which never
 * calls plants_init() -- see plants_init()'s own doc comment): yields an
 * empty plants_table_t rather than touching an uninitialised mutex. */
void plants_snapshot(plants_table_t *out);

/* Mutations (persisting; ESP_ERR_NOT_FOUND for unknown id / INVALID_ARG).
 * Like plants_snapshot() above, every one of these is also safe to call
 * before plants_init() has run -- they answer exactly as if the id given
 * doesn't exist (ESP_ERR_NOT_FOUND, or 0 for plants_create()'s "full"
 * contract), never touching an uninitialised mutex. */
esp_err_t plants_rename(uint8_t id, const char *name);
esp_err_t plants_assign(uint8_t id, const uint8_t *mac_or_null);
uint8_t   plants_create(void);                       /* 0 = full (or uninitialised) */
esp_err_t plants_delete(uint8_t id);                 /* also deletes P<id> ring files */

/* Auto-create sweep (final M8 review, H1+M3): adopts every LIVE registry mac
 * not already claimed by a plant into a new one (plants_resolve_or_create()),
 * off a registry SNAPSHOT the caller already took -- `reg` must come from a
 * registry snapshot (data_core_snapshot()), NEVER a request-supplied mac.
 * That boundary is load-bearing, not stylistic: this is the only sanctioned
 * driver of plant auto-creation, and every caller is either a periodic
 * internal tick or an open, unauthenticated GET (api_v1.c's plants_get) --
 * letting either seed the sweep from anything other than macs the radio
 * itself has already put in the registry would let a remote caller grow the
 * 16-slot plant table for free (the exact hole plants_history_get()'s
 * comment in api_v1.c documents and removes elsewhere).
 *
 * Liveness-gated exactly like the sampler's own per-plant sampling loop
 * (`now_uptime_s - e->last_seen_s > liveness_s` skips it): a LIVE unassigned
 * probe still gets auto-adopted -- that is the zero-config spec's decision
 * -- but a DEAD one no longer is. Without this gate, a probe that's been
 * swapped out or physically removed still sits in the registry forever
 * (registry.c never evicts an entry), so an unguarded sweep would re-adopt
 * it into a fresh, undeletable ghost plant on every call, burning a
 * never-reused id each time (plants_table.h) until the table permanently
 * exhausts. Gating on liveness is also what makes "swap the probe, unassign
 * the old plant" durable: once the dead mac goes stale it stops being
 * eligible for re-adoption, instead of clawing its way back into a plant on
 * the very next sweep.
 *
 * Bounded REGISTRY_MAX_SENSORS iterations, allocation-free (aside from the
 * plants_snapshot()/plants_resolve_or_create() calls it already makes). Safe
 * to call from any task context -- see plants_resolve_or_create()'s own
 * TASK CONTEXT ONLY note, which this inherits. */
void plants_adopt_from_registry(const registry_t *reg, uint32_t now_uptime_s, uint32_t liveness_s);

/* Probe-less last values: the plant's last history record (ring tail),
 * read via storage_query() (storage.h) over the plant's own P<id>_raw.bin
 * ring -- id-keyed since M8 Task 3, so this works for a plant with no
 * assigned sensor, or one whose probe was unplugged/reassigned elsewhere,
 * exactly the case this function exists to serve (a mac-keyed lookup would
 * have no mac to query by, or would go stale the moment
 * plants_table_assign() moves that mac to a different plant).
 *
 * Cached in RAM after first read (found or not), invalidated only by
 * plants_delete() -- deliberately not refreshed on every call: a probe-less
 * plant's ring is static (nothing appends to it once its probe is gone), so
 * the first scan's result stays correct until the plant itself is deleted.
 * Returns false when the plant has no history (id unknown, no ring file
 * yet, or the ring is empty). Fields use storage.h NONE sentinels. */
bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,
                        uint32_t *lux, uint16_t *conductivity, uint8_t *battery,
                        uint32_t *epoch_out);
