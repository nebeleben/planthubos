#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "capability.h"

#define HISTORY_COLS 8

typedef struct __attribute__((packed)) {
    uint16_t boot_id;              /* 0xFFFF = empty slot */
    uint32_t rel_s;
    int16_t  col[HISTORY_COLS];    /* CAP_VALUE_NONE = no value */
} storage_rec_t;                   /* exactly 22 bytes */
_Static_assert(sizeof(storage_rec_t) == 22, "history record must stay 22 bytes");

/* Per-plant column map, persisted in each ring file's header. cap[i] is the
 * capability id (capability.h) stored in col[i], or CAP_NONE if that column
 * is still unused. Pure, host-testable (no file I/O) -- see history_map_*
 * below and tests/host/test_history_cols.c. */
typedef struct { uint8_t fmt; uint8_t cap[HISTORY_COLS]; } history_map_t;  /* fmt=2 */

void history_map_init(history_map_t *m);                       /* all CAP_NONE, fmt=2 */
int  history_map_col(const history_map_t *m, uint8_t cap_id);  /* -1 if not mapped */
/* Returns the column for cap_id, allocating the first free one if needed;
 * -1 when the map is full (caller logs; live values are unaffected). */
int  history_map_ensure(history_map_t *m, uint8_t cap_id);

/* Retention is Kconfig-driven (components/storage/Kconfig); the host test
 * build (tests/host/run.sh, plain `cc`, no sdkconfig.h) never defines the
 * CONFIG_ symbols, so fall back to the same values as their Kconfig
 * defaults. */
#ifndef CONFIG_PLANTHUB_HISTORY_RAW_CAP
#define CONFIG_PLANTHUB_HISTORY_RAW_CAP 2880
#endif
#ifndef CONFIG_PLANTHUB_HISTORY_HOURLY_CAP
#define CONFIG_PLANTHUB_HISTORY_HOURLY_CAP 720
#endif
#define STORAGE_RAW_CAP     CONFIG_PLANTHUB_HISTORY_RAW_CAP
#define STORAGE_HOURLY_CAP  CONFIG_PLANTHUB_HISTORY_HOURLY_CAP

typedef enum { STORAGE_TIER_RAW, STORAGE_TIER_HOURLY } storage_tier_t;

typedef bool (*storage_resolve_fn)(void *rctx, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
typedef void (*storage_row_fn)(void *ctx, uint32_t epoch, const storage_rec_t *rec);

int  storage_append(const char *base, uint8_t plant_id, storage_tier_t tier, const storage_rec_t *rec);
/* map_out (may be NULL): filled with the ring file's persisted column map
 * so callers can interpret rec->col[i] -- an all-CAP_NONE map when the
 * file doesn't exist yet (nothing queried) or its header can't be trusted
 * (see storage.c's open_or_create()/read_header() doc comments). */
int  storage_query(const char *base, uint8_t plant_id, storage_tier_t tier,
                   uint32_t from_epoch, uint32_t to_epoch,
                   storage_resolve_fn resolve, void *rctx,
                   storage_row_fn row, void *ctx,
                   history_map_t *map_out);

/* Ensures cap_id has a column in plant_id's tier ring, allocating and
 * persisting the first free one into the file's header if it doesn't have
 * one yet (creating the ring file, with a fresh empty header, if it
 * doesn't exist at all). Returns the column, or -1 if the map is already
 * full (8/8 capabilities mapped) -- same contract as history_map_ensure(),
 * caller logs and drops the value; existing columns are unaffected. */
int  storage_col_for(const char *base, uint8_t plant_id, storage_tier_t tier, uint8_t cap_id);

void storage_reset_cache(void);

/* Frees plant_id's write-cursor cache slot(s) (both tiers, if present) for
 * reuse by a different plant id. Plant ids are never reused (plants_table.h)
 * and the per-tier cache is small and fixed-size (CACHE_SLOTS, storage.c) --
 * without this, deleting a plant would permanently strand its slot(s), and
 * repeated create/delete cycles past CACHE_SLOTS/2 distinct ids would
 * eventually leave no free slot for ANY plant's appends, live or not (a
 * cache_get() miss returns NULL, and storage_append() then fails outright).
 * Does NOT touch the on-disk ring files themselves -- callers that also
 * want those gone (e.g. plants_delete()) remove them separately; a later
 * storage_append() for the same id (which shouldn't happen, ids aren't
 * reused, but is harmless if it somehow did) would simply re-scan the file
 * for its write cursor, same as after storage_reset_cache(). Safe to call
 * for an id with no cached slot (a plant that never appended anything) --
 * a no-op. */
void storage_drop(uint8_t plant_id);
