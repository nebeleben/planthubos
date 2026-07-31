#include "hourly_agg.h"
#include <string.h>

void hourly_agg_init(hourly_agg_t *a) { memset(a, 0, sizeof(*a)); }

static void emit(const hourly_agg_t *a, storage_rec_t *out)
{
    memset(out, 0xFF, sizeof(*out));
    out->temp_dc = STORAGE_TEMP_NONE;
    out->boot_id = a->boot_id;
    out->rel_s = a->bucket * 3600;
    if (a->n_temp)  out->temp_dc = (int16_t)(a->sum_temp / (int32_t)a->n_temp);
    if (a->n_moist) out->moisture_pct = (uint8_t)(a->sum_moist / a->n_moist);
    if (a->n_batt)  out->battery_pct = (uint8_t)(a->sum_batt / a->n_batt);
    if (a->n_lux)   out->lux = (uint32_t)(a->sum_lux / a->n_lux);
    if (a->n_cond)  out->conductivity_us = (uint16_t)(a->sum_cond / a->n_cond);
}

static void accumulate(hourly_agg_t *a, const storage_rec_t *rec)
{
    if (rec->temp_dc != STORAGE_TEMP_NONE)          { a->sum_temp += rec->temp_dc; a->n_temp++; }
    if (rec->moisture_pct != STORAGE_U8_NONE)       { a->sum_moist += rec->moisture_pct; a->n_moist++; }
    if (rec->battery_pct != STORAGE_U8_NONE)        { a->sum_batt += rec->battery_pct; a->n_batt++; }
    if (rec->lux != STORAGE_LUX_NONE)               { a->sum_lux += rec->lux; a->n_lux++; }
    if (rec->conductivity_us != STORAGE_U16_NONE)   { a->sum_cond += rec->conductivity_us; a->n_cond++; }
}

bool hourly_agg_add(hourly_agg_t *a, const storage_rec_t *rec, storage_rec_t *out)
{
    uint32_t bucket = rec->rel_s / 3600;
    bool emitted = false;

    if (a->open && (rec->boot_id != a->boot_id || bucket != a->bucket)) {
        emit(a, out);
        emitted = true;
        a->open = false;
    }
    if (!a->open) {
        uint16_t b = rec->boot_id; uint32_t bk = bucket;
        hourly_agg_init(a);
        a->open = true; a->boot_id = b; a->bucket = bk;
    }
    accumulate(a, rec);
    return emitted;
}
