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

    /* --- coalesce: same MEASUREMENT (device+cap) collapses to one, newest value --- */
    {
        swarm_buf_t bc; swarm_buf_init(&bc);
        swarm_out_t m1 = { .tag = SWARM_OUT_MEASUREMENT, .u.meas = { .dev = { .kind = 2, .addr = {9} }, .cap_id = 2, .value = 10.0f, .age_s = 0 } };
        swarm_out_t m2 = m1; m2.u.meas.value = 20.0f;   /* same dev+cap, newer value */
        assert(swarm_buf_push_coalesce(&bc, &m1, 1000) == false);  /* first: appended */
        assert(swarm_buf_push_coalesce(&bc, &m2, 2000) == true);   /* second: coalesced */
        assert(swarm_buf_count(&bc) == 1);
        swarm_buf_entry_t e; assert(swarm_buf_pop(&bc, &e));
        assert(e.r.u.meas.value == 20.0f && e.captured_us == 2000);
        assert(swarm_buf_count(&bc) == 0);
    }
    /* --- coalesce keys on cap_id and on the device address --- */
    {
        swarm_buf_t bc; swarm_buf_init(&bc);
        swarm_out_t a = { .tag = SWARM_OUT_MEASUREMENT, .u.meas = { .dev = { .kind = 2, .addr = {9} }, .cap_id = 2, .value = 1.0f } };
        swarm_out_t b_cap = a; b_cap.u.meas.cap_id = 3;          /* different cap */
        swarm_out_t c_dev = a; c_dev.u.meas.dev.addr[0] = 8;     /* different device */
        assert(swarm_buf_push_coalesce(&bc, &a, 1) == false);
        assert(swarm_buf_push_coalesce(&bc, &b_cap, 2) == false); /* not coalesced */
        assert(swarm_buf_push_coalesce(&bc, &c_dev, 3) == false); /* not coalesced */
        assert(swarm_buf_count(&bc) == 3);
    }
    /* --- coalesce: same sensor READING (by MAC) collapses to one --- */
    {
        swarm_buf_t bc; swarm_buf_init(&bc);
        swarm_out_t r1 = make_reading(7, 0); r1.u.reading.temp_dc = 100;
        swarm_out_t r2 = make_reading(7, 0); r2.u.reading.temp_dc = 200; /* same mac[5]=7 */
        swarm_out_t r3 = make_reading(8, 0);                             /* different mac */
        assert(swarm_buf_push_coalesce(&bc, &r1, 1) == false);
        assert(swarm_buf_push_coalesce(&bc, &r2, 2) == true);
        assert(swarm_buf_push_coalesce(&bc, &r3, 3) == false);
        assert(swarm_buf_count(&bc) == 2);
        swarm_buf_entry_t e; assert(swarm_buf_pop(&bc, &e));   /* oldest slot = the coalesced r1/r2 */
        assert(e.r.u.reading.temp_dc == 200);
    }

    printf("test_swarm_buf: OK\n");
    return 0;
}
