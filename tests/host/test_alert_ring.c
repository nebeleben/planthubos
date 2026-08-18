/* tests/host/test_alert_ring.c -- the pure ring/collapsing decision behind
 * alert_post() (M5b Task 11, see alert_rec_t's doc comment in alert.h for
 * the rule this pins: consecutive identical (code, dev_idx, action_id)
 * pushes collapse into one entry with a repeat count, so a guard-refusal
 * burst against one actuator cannot evict every other alert from the
 * 8-entry ring). */
#include "alert.h"
#include <assert.h>
#include <stdio.h>

static alert_rec_t rec(uint8_t level, uint8_t code, int8_t dev_idx, uint8_t action_id, uint16_t param)
{
    alert_rec_t r;
    r.level = level;
    r.code = code;
    r.dev_idx = dev_idx;
    r.action_id = action_id;
    r.param = param;
    r.repeat = 0; /* alert_ring_push() must ignore this on input */
    return r;
}

/* A fresh push (nothing to collapse into) always lands with repeat == 1 --
 * "happened once", not "happened zero more times than itself". */
static void test_single_push_repeat_is_one(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t in = rec(1, 3, 2, 1, 10);
    alert_ring_push(&r, &in);
    assert(r.count == 1);
    assert(r.recs[0].repeat == 1);
    assert(r.recs[0].code == 3);
}

/* Three consecutive identical (code, dev_idx, action_id) pushes collapse
 * into ONE ring entry with repeat == 3 -- the whole point: a
 * max_per_hour=1 burst must not consume three of the ring's eight slots. */
static void test_consecutive_identical_collapse(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t a = rec(1, 4, 3, 2, 5);
    alert_ring_push(&r, &a);
    alert_ring_push(&r, &a);
    alert_ring_push(&r, &a);
    assert(r.count == 1);
    assert(r.recs[0].repeat == 3);
}

/* level/param on the collapsed entry reflect the MOST RECENT push, not the
 * first -- "how bad is this right now" wants the latest, not the original. */
static void test_collapse_keeps_latest_level_and_param(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t a = rec(1 /* NOTIFY */, 4, 3, 2, 5);
    alert_rec_t b = rec(2 /* ALERT */, 4, 3, 2, 99);
    alert_ring_push(&r, &a);
    alert_ring_push(&r, &b);
    assert(r.count == 1);
    assert(r.recs[0].repeat == 2);
    assert(r.recs[0].level == 2);
    assert(r.recs[0].param == 99);
}

/* Different identity (any of code/dev_idx/action_id) breaks the run: two
 * separate entries, neither collapsed. */
static void test_different_identity_does_not_collapse(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t a = rec(1, 4, 3, 2, 5);
    alert_rec_t b = rec(1, 4, 3, 1 /* different action_id */, 5);
    alert_ring_push(&r, &a);
    alert_ring_push(&r, &b);
    assert(r.count == 2);
    assert(r.recs[0].repeat == 1);
    assert(r.recs[1].repeat == 1);
}

/* Identical alerts with something else in between are NOT collapsed --
 * only the run against the ring's most-recently-pushed entry counts. */
static void test_non_consecutive_identical_does_not_collapse(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t a = rec(1, 4, 3, 2, 5);
    alert_rec_t mid = rec(1, 9, 0, 1, 0);
    alert_ring_push(&r, &a);
    alert_ring_push(&r, &mid);
    alert_ring_push(&r, &a);
    assert(r.count == 3);
    assert(r.recs[0].repeat == 1);
    assert(r.recs[1].repeat == 1);
    assert(r.recs[2].repeat == 1);
}

/* Ring-full behaviour is unchanged for a genuinely NEW entry: oldest is
 * evicted and counted in `dropped`. */
static void test_full_ring_evicts_oldest_on_new_entry(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    for (uint8_t i = 0; i < ALERT_RING_LEN; i++) {
        alert_rec_t a = rec(1, i, 0, 0, 0);
        alert_ring_push(&r, &a);
    }
    assert(r.count == ALERT_RING_LEN);
    assert(r.dropped == 0);

    alert_rec_t extra = rec(1, 200, 0, 0, 0);
    alert_ring_push(&r, &extra);
    assert(r.count == ALERT_RING_LEN);
    assert(r.dropped == 1);
    /* code=0 (the oldest) is gone; the newest push is present. */
    bool saw_zero = false, saw_extra = false;
    for (uint8_t i = 0; i < ALERT_RING_LEN; i++) {
        if (r.recs[i].code == 0) saw_zero = true;
        if (r.recs[i].code == 200) saw_extra = true;
    }
    assert(!saw_zero);
    assert(saw_extra);
}

/* A repeat burst against the ring's current tail entry while the ring is
 * ALREADY full must collapse in place -- no eviction, `dropped` unchanged
 * -- since this is exactly the case the whole feature exists for (a
 * guard-refusal burst must not evict unrelated alerts). */
static void test_full_ring_still_collapses_repeat_of_tail(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    for (uint8_t i = 0; i < ALERT_RING_LEN; i++) {
        alert_rec_t a = rec(1, i, 0, 0, 0);
        alert_ring_push(&r, &a);
    }
    /* Ring's tail is code=ALERT_RING_LEN-1. Repeat it three more times. */
    alert_rec_t tail = rec(1, ALERT_RING_LEN - 1, 0, 0, 0);
    alert_ring_push(&r, &tail);
    alert_ring_push(&r, &tail);
    alert_ring_push(&r, &tail);
    assert(r.count == ALERT_RING_LEN);
    assert(r.dropped == 0);
    /* Every original entry (code=0..LEN-1) is still present. */
    for (uint8_t i = 0; i < ALERT_RING_LEN; i++) {
        bool found = false;
        for (uint8_t j = 0; j < ALERT_RING_LEN; j++) {
            if (r.recs[j].code == i) { found = true; break; }
        }
        assert(found);
    }
}

/* repeat saturates at UINT16_MAX rather than wrapping to 0 -- the same
 * defensive spirit as actor_table.c's window_count saturation. */
static void test_repeat_saturates_not_wraps(void) {
    alert_ring_t r;
    alert_ring_init(&r);
    alert_rec_t a = rec(1, 4, 3, 2, 5);
    for (uint32_t i = 0; i < 70000; i++) alert_ring_push(&r, &a);
    assert(r.count == 1);
    assert(r.recs[0].repeat == 0xFFFF);
}

int main(void) {
    test_single_push_repeat_is_one();
    test_consecutive_identical_collapse();
    test_collapse_keeps_latest_level_and_param();
    test_different_identity_does_not_collapse();
    test_non_consecutive_identical_does_not_collapse();
    test_full_ring_evicts_oldest_on_new_entry();
    test_full_ring_still_collapses_repeat_of_tail();
    test_repeat_saturates_not_wraps();
    printf("test_alert_ring: OK\n");
    return 0;
}
