#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "plants_table.h"

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
