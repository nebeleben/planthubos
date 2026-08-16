#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "plants_table.h"
#include "registry.h"          /* registry_t: plants_bind_device()/plants_cap_value() */
#include "registry_compat.h"   /* M2-SHIM: legacy_registry_t, see that header */

/* LittleFS-backed plant registry: a single blob at <storage_base>/plants.bin
 * (M8 hardware bring-up fix -- this used to be an NVS blob keyed "plants" in
 * namespace "planthub", the same namespace claim/wifi/swarm state lives in;
 * every mutation rewrote the whole blob, and NVS's copy-on-write semantics
 * turned that into 19 stale copies in a 24KB partition within one session,
 * forcing constant GC in the SAME partition swarm's own rarely-written state
 * lives in -- see plants.c's top-of-file comment for the full story). RAM
 * cache + mutex layered on top of plants_table.h's pure logic; every
 * mutating call below persists to <storage_base>/plants.bin before
 * returning, via a tmp-file-then-rename() write (rename is atomic on
 * LittleFS). NVS is touched only once, by a one-boot migration in
 * plants_init() that adopts an existing pre-fix NVS blob if plants.bin
 * doesn't exist yet -- steady state never writes NVS.
 *
 * The on-disk blob is an explicit packed mirror struct (format byte 2 +
 * next_id + 16 x {id, in_use:u8, mac[6], mac_valid:u8, name[33], cap_bound[8],
 * cap_dev[8]}), never a raw dump of plants_table_t -- the host struct's bool
 * fields and padding are not a stable on-disk shape across compilers/targets.
 * A wrong length or an unrecognised format byte is loudly logged and treated
 * as "start empty" -- this never fails boot. See plants.c's PLANTS_BLOB_FORMAT
 * comment for the format-1 -> format-2 (M2 Task 4) bump: no migration code,
 * a clean-start format change (task-4-report.md). */

/* Init: loads <storage_base>/plants.bin (missing/bad -> empty table, logged;
 * if also absent, a one-boot check adopts a pre-fix NVS blob if one exists),
 * then runs the sensor-keyed one-boot migration (Task 4). Call from main.c
 * after data_core_init() and the littlefs mount, HUB ROLE ONLY (nodes keep
 * no plants). storage_base is "/storage", or NULL when storage/littlefs is
 * unavailable -- persistence then becomes a RAM-only no-op (single WARN
 * here) and plants_delete()'s ring-file cleanup has nothing to remove and is
 * skipped rather than failing; in practice the hub always has storage, so
 * this is a mount-failure fallback, not a normal path. Never fails boot on
 * its own account (any read problem is log-and-continue, defaults to an
 * empty table); the only error this can genuinely return is mutex
 * allocation (ESP_ERR_NO_MEM), which main.c must also log-and-continue past,
 * same as every other storage-adjacent init call there -- ESP_ERROR_CHECK is
 * not appropriate here. */
esp_err_t plants_init(const char *storage_base /* "/storage", or NULL when storage unavailable */);

/* mac -> plant id, auto-creating (and persisting to plants.bin) on first
 * sight. TASK CONTEXT ONLY (may do file I/O). 0 = table full (logged
 * once/mac/boot). */
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
uint8_t   plants_create(void);                       /* 0 = full (or uninitialised) */
esp_err_t plants_delete(uint8_t id);                 /* also deletes P<id> ring files */

/* M2-SHIM: V1's single-probe-per-plant assignment (mac_valid/mac,
 * plants_table_assign()'s "assigning a mac already assigned elsewhere MOVES
 * it" semantics). The M2 model (below) replaces "assign a probe" with "bind
 * the capabilities that probe reports" -- api_v1.c's probe-assignment route
 * is the only remaining caller, and it is not rewired by this task (RULING-1,
 * task-4-report.md). DELETE once that route moves to plants_bind_device(). */
esp_err_t plants_assign(uint8_t id, const uint8_t *mac_or_null);   /* M2-SHIM */

/* plant_binding_t is declared in plants_table.h (included above). */

/* Bind one capability on `plant_id` to `dev` (dev == NULL clears just that
 * one). Persists to plants.bin like every other mutation in this header.
 * Returns false for an unknown plant id or an unrecognised cap_id, or
 * before plants_init() has run (same "uninitialised registry" contract as
 * every other mutator here). */
bool plants_bind_cap(uint8_t plant_id, uint8_t cap_id, const device_id_t *dev);

/* Bind every capability `dev` currently reports in `reg` (a registry
 * snapshot the caller already took -- e.g. data_core_snapshot()) -- the V2
 * shape of the old V1 "assign this probe to this plant" one-click flow, now
 * "bind everything it currently has" instead of a single mac. Binds and
 * persists once after the whole sweep, not once per capability. Returns the
 * number of capabilities bound: 0 when plant_id is unknown, dev isn't in
 * `reg`, or the device reports nothing yet (registry devices with every
 * cap_slot_t.valid == false, e.g. paired but silent this boot). */
int plants_bind_device(uint8_t plant_id, const device_id_t *dev, const registry_t *reg);

/* Copies plant_id's currently-bound capabilities into out[] (max
 * CAPABILITY_COUNT). Returns the count actually copied. */
size_t plants_bindings(uint8_t plant_id, plant_binding_t *out, size_t max);

/* Latest value for one bound capability, straight from `reg` (a snapshot
 * the caller already took, same as plants_bind_device() above -- this
 * function does no registry lookup of its own beyond indexing into `reg`).
 * age_s_out is how long ago (seconds, this call's own wall-clock read) the
 * registry slot was last updated -- callers use it the way sampler.c's old
 * "now - e->last_seen_s > interval_s" staleness check did, just per
 * capability instead of per device. Returns false when plant_id has no
 * binding for cap_id, the bound device isn't in `reg`, or its slot has no
 * value yet (cap_slot_t.valid == false). */
bool plants_cap_value(uint8_t plant_id, uint8_t cap_id, const registry_t *reg,
                      float *value_out, uint32_t *age_s_out);

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
 * Bounded REGISTRY_MAX_SENSORS mutex-guarded plants_resolve_or_create()
 * calls; allocation-free and -- deliberately -- takes NO plants_table_t
 * snapshot of its own (re-review fix after H1+M3 landed): this function
 * runs on two different task stacks (the sampler task, AND api_v1.c's
 * plants_get on the httpd task, which has a documented exhaustion history),
 * so a 673-byte plants_table_t local would be unsafe on either, and a
 * shared `static` one would race between them. plants_resolve_or_create()
 * already does its own find-or-create atomically under s_mutex, so there is
 * nothing to pre-check a snapshot against. Safe to call from any task
 * context -- see plants_resolve_or_create()'s own TASK CONTEXT ONLY note,
 * which this inherits. */
void plants_adopt_from_registry(const legacy_registry_t *reg, uint32_t now_uptime_s, uint32_t liveness_s);   /* M2-SHIM */

/* M2-SHIM: probe-less last values, V1's fixed 5-field shape (storage_compat.h's
 * storage_rec_v1_t) rather than plants_cap_value()'s per-capability one.
 * sensors_json.c is the only remaining caller and is not rewired by this
 * task (RULING-1, task-4-report.md) -- DELETE once it moves to
 * plants_bindings()/plants_cap_value(). Kept working exactly as before:
 * the plant's last history record (ring tail),
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
bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,   /* M2-SHIM */
                        uint32_t *lux, uint16_t *conductivity, uint8_t *battery,
                        uint32_t *epoch_out);
