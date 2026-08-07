#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_RAW_CAP     2880
#define STORAGE_HOURLY_CAP  720

#define STORAGE_TEMP_NONE   INT16_MIN
#define STORAGE_U8_NONE     0xFF
#define STORAGE_LUX_NONE    0xFFFFFFFFu
#define STORAGE_U16_NONE    0xFFFF

typedef struct __attribute__((packed)) {
    uint16_t boot_id;          /* 0xFFFF = empty slot */
    uint32_t rel_s;            /* uptime seconds at sample time */
    int16_t  temp_dc;
    uint8_t  moisture_pct;
    uint8_t  battery_pct;
    uint32_t lux;
    uint16_t conductivity_us;
} storage_rec_t;               /* exactly 16 bytes */

typedef enum { STORAGE_TIER_RAW, STORAGE_TIER_HOURLY } storage_tier_t;

typedef bool (*storage_resolve_fn)(void *rctx, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
typedef void (*storage_row_fn)(void *ctx, uint32_t epoch, const storage_rec_t *rec);

int  storage_append(const char *base, uint8_t plant_id, storage_tier_t tier, const storage_rec_t *rec);
int  storage_query(const char *base, uint8_t plant_id, storage_tier_t tier,
                   uint32_t from_epoch, uint32_t to_epoch,
                   storage_resolve_fn resolve, void *rctx,
                   storage_row_fn row, void *ctx);
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
