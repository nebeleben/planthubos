#include "hourly_agg.h"
#include <string.h>

void hourly_agg_init(hourly_agg_t *a) { memset(a, 0, sizeof(*a)); }

static void emit(const hourly_agg_t *a, storage_rec_t *out)
{
    out->boot_id = a->boot_id;
    out->rel_s = a->bucket * 3600;
    for (int i = 0; i < HISTORY_COLS; i++)
        out->col[i] = a->n[i] ? (int16_t)(a->sum[i] / (int64_t)a->n[i]) : CAP_VALUE_NONE;
}

static void accumulate(hourly_agg_t *a, const storage_rec_t *rec)
{
    for (int i = 0; i < HISTORY_COLS; i++) {
        if (rec->col[i] != CAP_VALUE_NONE) {
            a->sum[i] += rec->col[i];
            a->n[i]++;
        }
    }
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
