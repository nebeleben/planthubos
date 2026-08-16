#pragma once
#include <stdbool.h>
#include "storage.h"

typedef struct {
    bool     open;
    uint16_t boot_id;
    uint32_t bucket;                  /* rel_s / 3600 */
    uint32_t n[HISTORY_COLS];
    int64_t  sum[HISTORY_COLS];
} hourly_agg_t;

void hourly_agg_init(hourly_agg_t *a);
bool hourly_agg_add(hourly_agg_t *a, const storage_rec_t *rec, storage_rec_t *out);
