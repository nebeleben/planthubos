#pragma once
#include <stdbool.h>
#include "storage.h"

typedef struct {
    bool     open;
    uint16_t boot_id;
    uint32_t bucket;        /* rel_s / 3600 */
    uint32_t n_temp, n_moist, n_batt, n_lux, n_cond;
    int32_t  sum_temp;
    uint32_t sum_moist, sum_batt, sum_cond;
    uint64_t sum_lux;
} hourly_agg_t;

void hourly_agg_init(hourly_agg_t *a);
bool hourly_agg_add(hourly_agg_t *a, const storage_rec_t *rec, storage_rec_t *out);
