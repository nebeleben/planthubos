#pragma once
#include "storage.h"

/* M2-SHIM: bridges the V1 fixed-field history record (temp/moisture/
 * battery/lux/conductivity, storage_rec_t's old 16-byte shape) onto the
 * V2 capability-mapped storage_rec_t (storage.h) for the three RULING-1
 * consumers of the history storage API -- sampler.c (writer),
 * plants.c's plants_last_values() (reader), and webserver/api_v1.c's
 * history route (reader) -- until Tasks 4/6 rewire them directly onto
 * capability ids.
 *
 * Both helpers below always map the same 5 legacy capabilities
 * (air.temperature, soil.moisture, battery.level, light.illuminance,
 * soil.conductivity) in the same fixed order, onto BOTH the raw and
 * hourly tiers of a given plant -- so a plant's two ring files keep
 * identical column layouts. That matters because hourly_agg.c aggregates
 * generically by column POSITION, not by capability: sampler.c builds the
 * hourly record straight from the raw record's storage_rec_t (same
 * column layout) and appends it via the real storage_append(), never
 * going through this shim, so the two files must already agree on what
 * each column means before that happens.
 *
 * DELETE this file, storage_compat.c, and every M2-SHIM call site above
 * once Tasks 4/6 rewire those consumers onto storage_col_for()/
 * capability_encode()/capability_decode() directly. */

#define STORAGE_TEMP_NONE   INT16_MIN     /* M2-SHIM */
#define STORAGE_U8_NONE     0xFF          /* M2-SHIM */
#define STORAGE_LUX_NONE    0xFFFFFFFFu   /* M2-SHIM */
#define STORAGE_U16_NONE    0xFFFF        /* M2-SHIM */

typedef struct {                          /* M2-SHIM: V1 field shape */
    uint16_t boot_id;
    uint32_t rel_s;
    int16_t  temp_dc;            /* deci-C; STORAGE_TEMP_NONE = no value */
    uint8_t  moisture_pct;       /* STORAGE_U8_NONE = no value */
    uint8_t  battery_pct;        /* STORAGE_U8_NONE = no value */
    uint32_t lux;                /* raw lux; STORAGE_LUX_NONE = no value */
    uint16_t conductivity_us;    /* STORAGE_U16_NONE = no value */
} storage_rec_v1_t;

typedef void (*storage_row_v1_fn)(void *ctx, uint32_t epoch, const storage_rec_v1_t *rec);

/* M2-SHIM: encodes rec's 5 legacy fields into *out (a real V2
 * storage_rec_t), allocating/persisting their columns on both tiers'
 * ring files for plant_id if this is the first time any of them is seen.
 * Does not append -- callers pass *out straight to the real
 * storage_append()/hourly_agg_add(), unchanged. */
void storage_encode_v1(const char *base, uint8_t plant_id,
                        const storage_rec_v1_t *rec, storage_rec_t *out);

/* M2-SHIM: like storage_query(), decoding each row back into the V1 shape
 * via whatever column map the ring file's own header actually has --
 * independent of this call's own ensure-order, so it also correctly reads
 * files a future direct capability writer (Task 4/6) populated. */
int storage_query_v1(const char *base, uint8_t plant_id, storage_tier_t tier,
                      uint32_t from_epoch, uint32_t to_epoch,
                      storage_resolve_fn resolve, void *rctx,
                      storage_row_v1_fn row, void *ctx);
