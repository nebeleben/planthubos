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

/* Read-only snapshot for the API/MQTT/UI. */
void plants_snapshot(plants_table_t *out);

/* Mutations (persisting; ESP_ERR_NOT_FOUND for unknown id / INVALID_ARG). */
esp_err_t plants_rename(uint8_t id, const char *name);
esp_err_t plants_assign(uint8_t id, const uint8_t *mac_or_null);
uint8_t   plants_create(void);                       /* 0 = full */
esp_err_t plants_delete(uint8_t id);                 /* also deletes P<id> ring files */

/* Probe-less last values: the plant's last history record (ring tail),
 * cached in RAM after first read; invalidated by plants_delete. Returns
 * false when the plant has no history. Fields use storage.h NONE sentinels.
 *
 * TASK 3 REKEYS THIS: switch to plant_id. storage_query() (storage.h) is
 * still MAC-keyed today, not plant-keyed -- and a probe-less plant (exactly
 * the case this function exists to serve: a plant with no assigned sensor,
 * or one whose probe was unplugged) has no mac to query by at all. Querying
 * under whatever mac a plant last happened to have assigned would also go
 * silently stale/wrong the moment that mac is reassigned to a different
 * plant (plants_table_assign() allows exactly that move). Rather than ship
 * either a wrong answer or an answer that only works for currently-assigned
 * plants, this is stubbed to always return false until Task 3 rekeys
 * storage.c's on-disk files to plant_id and this can query directly. */
bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,
                        uint32_t *lux, uint16_t *conductivity, uint8_t *battery,
                        uint32_t *epoch_out);
