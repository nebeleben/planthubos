#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "swarm_buf.h"

static swarm_out_t make_reading(uint8_t tag, uint16_t age_s)
{
    swarm_out_t o;
    memset(&o, 0, sizeof(o));
    o.tag = SWARM_OUT_READING;
    o.u.reading.version = SWARM_PROTO_VERSION;
    o.u.reading.type = SWARM_MSG_READING;
    o.u.reading.frame_cnt = tag;
    o.u.reading.mac[5] = tag;         /* distinguishes entries for the FIFO-order check */
    o.u.reading.temp_dc = INT16_MIN;
    o.u.reading.moisture_pct = 0xFF;
    o.u.reading.battery_pct = 0xFF;
    o.u.reading.lux = 0xFFFFFFFFu;
    o.u.reading.conductivity_us = 0xFFFF;
    o.u.reading.age_s = age_s;
    return o;
}

int main(void)
{
    /* --- fill, exactly to capacity: no eviction yet --- */
    swarm_buf_t b;
    swarm_buf_init(&b);
    assert(swarm_buf_count(&b) == 0);
    assert(swarm_buf_dropped(&b) == 0);

    for (int i = 0; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_out_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 0);

    /* --- FIFO order: pop must return entries in push order --- */
    for (int i = 0; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.u.reading.frame_cnt == (uint8_t)i);
        assert(e.r.u.reading.mac[5] == (uint8_t)i);
    }
    assert(swarm_buf_count(&b) == 0);
    {
        swarm_buf_entry_t empty_check;
        assert(!swarm_buf_pop(&b, &empty_check));  /* empty: pop fails, dropped stays 0 */
    }
    assert(swarm_buf_dropped(&b) == 0);

    /* --- overwrite-oldest at capacity --- */
    swarm_buf_init(&b);
    for (int i = 0; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_out_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    /* One more push while full: must evict entry 0 (the oldest), not any
     * other slot, and count must NOT grow past capacity. */
    swarm_out_t overflow = make_reading(0xAA, 0);
    swarm_buf_push(&b, &overflow, 2000);
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 1);

    /* FIFO order after the eviction: entry 1..N-1 (in that order), then the
     * new entry 0xAA last -- entry 0 must be gone entirely. */
    for (int i = 1; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.u.reading.frame_cnt == (uint8_t)i);
    }
    swarm_buf_entry_t last;
    assert(swarm_buf_pop(&b, &last));
    assert(last.r.u.reading.frame_cnt == 0xAA);
    assert(swarm_buf_count(&b) == 0);

    /* A second overflow push, immediately after the first, confirms the
     * dropped counter keeps accumulating rather than resetting. */
    swarm_buf_init(&b);
    for (int i = 0; i < SWARM_NODE_BUFFER_LEN + 3; i++) {
        swarm_out_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 3);
    /* The 3 oldest (frame_cnt 0,1,2) were evicted; the surviving FIFO order
     * starts at frame_cnt 3. */
    {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.u.reading.frame_cnt == 3);
    }

    /* --- age recomputation --- */
    /* No elapsed time: age unchanged. */
    assert(swarm_buf_recompute_age(5, 1000000, 1000000) == 5);
    /* 3s elapsed (3,000,000 us), base age 2s -> 5s total. */
    assert(swarm_buf_recompute_age(2, 1000000, 4000000) == 5);
    /* Sub-second elapsed truncates to 0 additional seconds. */
    assert(swarm_buf_recompute_age(7, 1000000, 1999999) == 7);
    /* now_us before captured_us (clock oddity): adds nothing, never underflows. */
    assert(swarm_buf_recompute_age(9, 5000000, 1000000) == 9);

    /* --- age clamp at UINT16_MAX --- */
    /* base already at the max: any further elapsed still clamps, doesn't wrap. */
    assert(swarm_buf_recompute_age(UINT16_MAX, 0, 10000000) == UINT16_MAX);
    /* base just below the max, plus enough elapsed to cross it: clamps, not wraps. */
    {
        int64_t captured = 0;
        int64_t now = ((int64_t)20) * 1000000;  /* +20s */
        uint16_t got = swarm_buf_recompute_age((uint16_t)(UINT16_MAX - 5), captured, now);
        assert(got == UINT16_MAX);
    }

    /* --- the union carries a MEASUREMENT frame too, not just READING --- */
    {
        swarm_buf_t bm; swarm_buf_init(&bm);
        swarm_out_t m = { .tag = SWARM_OUT_MEASUREMENT, .u.meas = { .dev = { .kind = 2, .addr = {1} }, .cap_id = 2, .value = 3.5f, .age_s = 0 } };
        swarm_buf_push(&bm, &m, 1000000);
        swarm_buf_entry_t e; assert(swarm_buf_pop(&bm, &e));
        assert(e.r.tag == SWARM_OUT_MEASUREMENT && e.r.u.meas.value == 3.5f);
        assert(swarm_buf_recompute_age(e.r.u.meas.age_s, e.captured_us, 4000000) == 3);
    }

    printf("test_swarm_buf: OK\n");
    return 0;
}
