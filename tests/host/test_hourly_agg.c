#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hourly_agg.h"

/* col 0 stands in for a "temp"-like always-present reading, col 1 for a
 * "moisture"-like one that's sometimes absent (CAP_VALUE_NONE), col 2 is
 * never written at all -- hourly_agg.c is fully generic over column
 * position, so no capability ids are needed to exercise it. */
static storage_rec_t mk(uint16_t boot, uint32_t rel, int16_t c0, int16_t c1)
{
    storage_rec_t r;
    memset(&r, 0xFF, sizeof(r));
    r.boot_id = boot; r.rel_s = rel;
    for (int i = 0; i < HISTORY_COLS; i++) r.col[i] = CAP_VALUE_NONE;
    r.col[0] = c0;
    r.col[1] = c1;
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
    r = mk(1, 2000, 240, CAP_VALUE_NONE);   /* col 1 absent this sample */
    assert(!hourly_agg_add(&a, &r, &out));

    /* first sample of hour 1 emits hour 0's averages */
    r = mk(1, 3700, 300, 50);
    assert(hourly_agg_add(&a, &r, &out));
    assert(out.boot_id == 1 && out.rel_s == 0);
    assert(out.col[0] == 220);              /* (200+220+240)/3 */
    assert(out.col[1] == 42);               /* (40+44)/2 - absent samples excluded */
    assert(out.col[2] == CAP_VALUE_NONE);   /* never present */

    /* boot change flushes the open bucket */
    r = mk(2, 10, 100, 10);
    assert(hourly_agg_add(&a, &r, &out));
    assert(out.boot_id == 1 && out.rel_s == 3600 && out.col[0] == 300);

    /* nothing pending emitted twice */
    r = mk(2, 20, 110, 12);
    assert(!hourly_agg_add(&a, &r, &out));

    printf("test_hourly_agg: OK\n");
    return 0;
}
