#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "swarm_buf.h"

static swarm_reading_t make_reading(uint8_t tag, uint16_t age_s)
{
    swarm_reading_t r;
    memset(&r, 0, sizeof(r));
    r.version = SWARM_PROTO_VERSION;
    r.type = SWARM_MSG_READING;
    r.frame_cnt = tag;
    r.mac[5] = tag;         /* distinguishes entries for the FIFO-order check */
    r.temp_dc = INT16_MIN;
    r.moisture_pct = 0xFF;
    r.battery_pct = 0xFF;
    r.lux = 0xFFFFFFFFu;
    r.conductivity_us = 0xFFFF;
    r.age_s = age_s;
    return r;
}

int main(void)
{
    /* --- fill, exactly to capacity: no eviction yet --- */
    swarm_buf_t b;
    swarm_buf_init(&b);
    assert(swarm_buf_count(&b) == 0);
    assert(swarm_buf_dropped(&b) == 0);

    for (int i = 0; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_reading_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 0);

    /* --- FIFO order: pop must return entries in push order --- */
    for (int i = 0; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.frame_cnt == (uint8_t)i);
        assert(e.r.mac[5] == (uint8_t)i);
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
        swarm_reading_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    /* One more push while full: must evict entry 0 (the oldest), not any
     * other slot, and count must NOT grow past capacity. */
    swarm_reading_t overflow = make_reading(0xAA, 0);
    swarm_buf_push(&b, &overflow, 2000);
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 1);

    /* FIFO order after the eviction: entry 1..N-1 (in that order), then the
     * new entry 0xAA last -- entry 0 must be gone entirely. */
    for (int i = 1; i < SWARM_NODE_BUFFER_LEN; i++) {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.frame_cnt == (uint8_t)i);
    }
    swarm_buf_entry_t last;
    assert(swarm_buf_pop(&b, &last));
    assert(last.r.frame_cnt == 0xAA);
    assert(swarm_buf_count(&b) == 0);

    /* A second overflow push, immediately after the first, confirms the
     * dropped counter keeps accumulating rather than resetting. */
    swarm_buf_init(&b);
    for (int i = 0; i < SWARM_NODE_BUFFER_LEN + 3; i++) {
        swarm_reading_t r = make_reading((uint8_t)i, 0);
        swarm_buf_push(&b, &r, 1000);
    }
    assert(swarm_buf_count(&b) == SWARM_NODE_BUFFER_LEN);
    assert(swarm_buf_dropped(&b) == 3);
    /* The 3 oldest (frame_cnt 0,1,2) were evicted; the surviving FIFO order
     * starts at frame_cnt 3. */
    {
        swarm_buf_entry_t e;
        assert(swarm_buf_pop(&b, &e));
        assert(e.r.frame_cnt == 3);
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

    printf("test_swarm_buf: OK\n");
    return 0;
}
