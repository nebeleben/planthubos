#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hourly_agg.h"

static storage_rec_t mk(uint16_t boot, uint32_t rel, int16_t temp, uint8_t moist)
{
    storage_rec_t r;
    memset(&r, 0xFF, sizeof(r));
    r.temp_dc = STORAGE_TEMP_NONE;
    r.boot_id = boot; r.rel_s = rel;
    if (temp != STORAGE_TEMP_NONE) r.temp_dc = temp;
    r.moisture_pct = moist;
    return r;
}

int main(void)
{
    hourly_agg_t a;
    hourly_agg_init(&a);
    storage_rec_t out;

    /* three samples in hour bucket 0 -> nothing emitted yet */
    storage_rec_t r = mk(1, 100, 200, 40);
    assert(!hourly_agg_add(&a, &r, &out));
    r = mk(1, 1000, 220, 44);
    assert(!hourly_agg_add(&a, &r, &out));
    r = mk(1, 2000, 240, STORAGE_U8_NONE);   /* moisture absent this sample */
    assert(!hourly_agg_add(&a, &r, &out));

    /* first sample of hour 1 emits hour 0's averages */
    r = mk(1, 3700, 300, 50);
    assert(hourly_agg_add(&a, &r, &out));
    assert(out.boot_id == 1 && out.rel_s == 0);
    assert(out.temp_dc == 220);              /* (200+220+240)/3 */
    assert(out.moisture_pct == 42);          /* (40+44)/2 - absent samples excluded */
    assert(out.lux == STORAGE_LUX_NONE);     /* never present */

    /* boot change flushes the open bucket */
    r = mk(2, 10, 100, 10);
    assert(hourly_agg_add(&a, &r, &out));
    assert(out.boot_id == 1 && out.rel_s == 3600 && out.temp_dc == 300);

    /* nothing pending emitted twice */
    r = mk(2, 20, 110, 12);
    assert(!hourly_agg_add(&a, &r, &out));

    printf("test_hourly_agg: OK\n");
    return 0;
}
