/* M2-SHIM: see storage_compat.h's doc comment. */
#include "storage_compat.h"
#include "capability.h"
#include <math.h>

/* Fixed legacy capability order -- ensures storage_col_for() allocates the
 * exact same columns, in the same order, every call (idempotent past the
 * first) and on both tiers, so a plant's raw and hourly ring files always
 * agree on what each column position means (see storage_compat.h). */
static const uint8_t LEGACY_CAPS[5] = {
    CAP_AIR_TEMPERATURE, CAP_SOIL_MOISTURE, CAP_BATTERY_LEVEL,
    CAP_LIGHT_ILLUMINANCE, CAP_SOIL_CONDUCTIVITY,
};

void storage_encode_v1(const char *base, uint8_t plant_id,
                        const storage_rec_v1_t *rec, storage_rec_t *out)
{
    out->boot_id = rec->boot_id;
    out->rel_s = rec->rel_s;
    for (int i = 0; i < HISTORY_COLS; i++) out->col[i] = CAP_VALUE_NONE;

    for (int i = 0; i < 5; i++) {
        uint8_t cap = LEGACY_CAPS[i];
        int col = storage_col_for(base, plant_id, STORAGE_TIER_RAW, cap);
        /* Also ensure the hourly tier's header maps the same capability,
         * even though nothing is written there this call -- keeps the two
         * files' column layouts in lockstep from the very first sample
         * (see storage_compat.h's doc comment). */
        (void)storage_col_for(base, plant_id, STORAGE_TIER_HOURLY, cap);
        if (col < 0) continue;   /* map full: value dropped, per storage_col_for()'s contract */

        bool present;
        float v;
        switch (cap) {
        case CAP_AIR_TEMPERATURE:
            present = rec->temp_dc != STORAGE_TEMP_NONE;
            v = (float)rec->temp_dc / 10.0f;
            break;
        case CAP_SOIL_MOISTURE:
            present = rec->moisture_pct != STORAGE_U8_NONE;
            v = (float)rec->moisture_pct;
            break;
        case CAP_BATTERY_LEVEL:
            present = rec->battery_pct != STORAGE_U8_NONE;
            v = (float)rec->battery_pct;
            break;
        case CAP_LIGHT_ILLUMINANCE:
            present = rec->lux != STORAGE_LUX_NONE;
            v = (float)rec->lux;
            break;
        default: /* CAP_SOIL_CONDUCTIVITY */
            present = rec->conductivity_us != STORAGE_U16_NONE;
            v = (float)rec->conductivity_us;
            break;
        }
        out->col[col] = present ? capability_encode(cap, v) : CAP_VALUE_NONE;
    }
}

typedef struct {
    storage_row_v1_fn row;
    void *ctx;
    history_map_t map;
} v1_row_ctx_t;

/* storage_query() fills c->map (via storage_query's own map_out parameter,
 * aliased here) before it ever calls this adapter, so the lookups below
 * always see the file's real, current column layout -- not this shim's
 * own ensure-order, which matters if a future direct-capability writer
 * (Task 4/6) laid the columns out differently. */
static void v1_row_adapter(void *vctx, uint32_t epoch, const storage_rec_t *rec)
{
    v1_row_ctx_t *c = vctx;
    storage_rec_v1_t v1 = {
        .boot_id = rec->boot_id,
        .rel_s = rec->rel_s,
        .temp_dc = STORAGE_TEMP_NONE,
        .moisture_pct = STORAGE_U8_NONE,
        .battery_pct = STORAGE_U8_NONE,
        .lux = STORAGE_LUX_NONE,
        .conductivity_us = STORAGE_U16_NONE,
    };

    int col;
    if ((col = history_map_col(&c->map, CAP_AIR_TEMPERATURE)) >= 0 && rec->col[col] != CAP_VALUE_NONE)
        v1.temp_dc = (int16_t)lroundf(capability_decode(CAP_AIR_TEMPERATURE, rec->col[col]) * 10.0f);
    if ((col = history_map_col(&c->map, CAP_SOIL_MOISTURE)) >= 0 && rec->col[col] != CAP_VALUE_NONE)
        v1.moisture_pct = (uint8_t)lroundf(capability_decode(CAP_SOIL_MOISTURE, rec->col[col]));
    if ((col = history_map_col(&c->map, CAP_BATTERY_LEVEL)) >= 0 && rec->col[col] != CAP_VALUE_NONE)
        v1.battery_pct = (uint8_t)lroundf(capability_decode(CAP_BATTERY_LEVEL, rec->col[col]));
    if ((col = history_map_col(&c->map, CAP_LIGHT_ILLUMINANCE)) >= 0 && rec->col[col] != CAP_VALUE_NONE)
        v1.lux = (uint32_t)lroundf(capability_decode(CAP_LIGHT_ILLUMINANCE, rec->col[col]));
    if ((col = history_map_col(&c->map, CAP_SOIL_CONDUCTIVITY)) >= 0 && rec->col[col] != CAP_VALUE_NONE)
        v1.conductivity_us = (uint16_t)lroundf(capability_decode(CAP_SOIL_CONDUCTIVITY, rec->col[col]));

    c->row(c->ctx, epoch, &v1);
}

int storage_query_v1(const char *base, uint8_t plant_id, storage_tier_t tier,
                      uint32_t from_epoch, uint32_t to_epoch,
                      storage_resolve_fn resolve, void *rctx,
                      storage_row_v1_fn row, void *ctx)
{
    v1_row_ctx_t c = { .row = row, .ctx = ctx };
    return storage_query(base, plant_id, tier, from_epoch, to_epoch,
                          resolve, rctx, v1_row_adapter, &c, &c.map);
}
