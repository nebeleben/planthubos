/* tests/host/test_actor_table.c */
#include "actor_table.h"
#include <assert.h>
#include <stdio.h>

static actor_table_t T;
static void setup(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 300, 0x01));
    /* The close too: test_lockout() asserts a safety close is permitted, and
     * an action the device does not declare would refuse as UNKNOWN long
     * before the lockout rule was ever consulted. */
    assert(actor_table_add(&T, 3, ACT_SWITCH_OFF, 0, 0x00));
}

static void test_bound_enforced(void) {
    setup();
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 300, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 301, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

/* A wrapper that declared a LOWER max than the firmware's is the effective
 * bound -- tightening is always allowed. Asserts BOTH sides: at the
 * tightened max it must still be OK (an off-by-one that tightened too far
 * would pass a refusal-only test), and just past it must be refused. */
static void test_wrapper_bound_tightens(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 60, 0x01));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 60, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 61, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

static void test_cooldown(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 60, /*max_per_hour*/ 10);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 159) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 160) == ACTOR_OK);
}

/* Fixed hourly window (spec section 4.2). The boundary behaviour is a
 * documented trade, so it is asserted rather than left to chance. Also
 * asserts the FIRST fire is permitted (a too-strict off-by-one like
 * `count >= max_per_hour - 1` would still fail the refusal-only checks
 * below but pass a suite that never checked the positive case). */
static void test_rate_limit_fixed_window(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 0, 2);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 200);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_RATE);
    /* New window opens 3600 s after the window START, not after the last fire. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 3700) == ACTOR_OK);
}

/* One budget per (device, action), whatever asked -- that is the whole
 * reason guards attach here and not to the plant or the rule. */
static void test_one_budget_across_sources(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 0, 2);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);   /* a rule */
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 110);   /* a manual press */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 120) == ACTOR_REFUSED_RATE);
}

/* Lockout is the operator's stop button: it refuses rules, permits manual,
 * and permits the safety close -- a lockout that blocked the close would
 * strand an actuator open. */
static void test_lockout(void) {
    setup();
    actor_table_set_lockout(&T, 3, true);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_LOCKOUT);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 100) == ACTOR_OK);
}

static void test_capacity_refuses_fifth_actor(void) {
    actor_table_init(&T);
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) assert(actor_table_add(&T, i, ACT_SWITCH_ON, 0, 0));
    assert(!actor_table_add(&T, 99, ACT_SWITCH_ON, 0, 0));
    assert(actor_table_full_drops(&T) == 1);
}

static void test_unknown_device_or_action(void) {
    setup();
    assert(actor_table_check(&T, 7, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, 3, ACT_PUMP_RUN, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    /* ACT_SWITCH_OFF IS declared by this device (see setup), so it must not
     * be refused as unknown -- this pins the distinction. */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
}

/* ---- Coverage beyond the brief's floor (see task-6 report) ---- */

/* Ordering: BOUND must win over LOCKOUT. A locked-out device given an
 * out-of-range parameter should report the bound violation, not the
 * lockout -- otherwise a user fixing the "lockout" refusal by waiting for
 * the operator to clear it would still hit BOUND next, for no visible
 * reason. */
static void test_bound_wins_over_lockout(void) {
    setup();
    actor_table_set_lockout(&T, 3, true);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 301, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

/* Ordering: COOLDOWN must win over RATE when both would refuse. */
static void test_cooldown_wins_over_rate(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);
    /* At t=150: still within the 600 s cooldown AND already at the 1/hour
     * cap -- must report COOLDOWN, per the fixed unknown->bound->lockout->
     * cooldown->rate order. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_COOLDOWN);
}

/* Zero means unlimited/off for BOTH guards, and that must hold even after
 * many recorded fires, not just when neither guard is configured. */
static void test_zero_guards_stay_unlimited(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 0, /*max_per_hour*/ 0);
    for (uint32_t t = 0; t < 50; t++)
        actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100 + t);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 149) == ACTOR_OK);
}

/* A cooldown configured before the pair has ever fired must not refuse:
 * window_count == 0 (never recorded) must not be confused with a fire at
 * last_fire_s == 0. */
static void test_cooldown_before_first_fire_permits(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 3600, /*max_per_hour*/ 0);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 0) == ACTOR_OK);
}

/* window_count is a uint8_t: it must saturate at 255 rather than wrap to 0
 * and silently reopen "capacity" mid-window. Uses a cooldown of 0 so only
 * the rate axis is exercised, and a max_per_hour above the saturation
 * point so the refusal, once reached, must never flip back to OK within
 * the same window. */
static void test_window_count_saturates_not_wraps(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 0, 255);
    for (int i = 0; i < 300; i++)
        actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100 + (uint32_t)i);
    /* Still inside the same 3600 s window as the first record at t=100. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 399) == ACTOR_REFUSED_RATE);
}

/* Review round 1, finding 1 (CRITICAL): re-declaring an already-tracked
 * pair -- a wrapper re-parse, a re-discovery pass, an API-driven wrapper
 * update -- must not erase an hourly budget already spent or reset the
 * operator's cooldown. Traces the reviewer's own repro: a cooldown earned
 * by two recorded fires must still be in force after an identical re-add. */
static void test_readd_preserves_guards_and_window(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 300, 0x01));
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 600, /*max_per_hour*/ 2);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 200);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_COOLDOWN);
    /* Re-declare the identical pair. */
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 300, 0x01));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_COOLDOWN);
}

/* Review round 1, finding 2 (Important): ACTOR_SRC_SAFETY is exempt from
 * cooldown -- a close-retry storm (spec section 4.3/4.5) must not be
 * refused by a guard that exists to rate-shape rules and manual presses,
 * not to block the one source whose job is closing something already
 * open. */
static void test_safety_exempt_from_cooldown(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_SWITCH_OFF, /*cooldown_s*/ 600, /*max_per_hour*/ 0);
    actor_table_record(&T, 3, ACT_SWITCH_OFF, 100);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 150) == ACTOR_OK);
}

/* Same finding, the rate axis: a user-configured hourly cap on switch.off
 * must not be able to strand an actuator open either. */
static void test_safety_exempt_from_rate(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_SWITCH_OFF, /*cooldown_s*/ 0, /*max_per_hour*/ 1);
    actor_table_record(&T, 3, ACT_SWITCH_OFF, 100);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_RATE);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 150) == ACTOR_OK);
}

/* Review round 1, finding 3 (Important): -1 is find_free_row()'s own
 * sentinel for an unused row, and also registry_find()'s canonical
 * not-found return -- a caller that resolved a device id to "not found"
 * must not have that treated as a legitimate device. All five entry
 * points reject a negative dev_idx; add()'s rejection must not be counted
 * in full_drops (that counter is for a genuinely full table, not bad
 * input -- see finding 4 / actor_table_add()'s header comment). */
static void test_negative_dev_idx_rejected(void) {
    actor_table_init(&T);
    assert(!actor_table_add(&T, -1, ACT_SWITCH_ON, 0, 0));
    assert(actor_table_full_drops(&T) == 0);
    assert(actor_table_check(&T, -1, ACT_SWITCH_ON, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    assert(!actor_table_set_guards(&T, -1, ACT_SWITCH_ON, 10, 1));
    actor_table_record(&T, -1, ACT_SWITCH_ON, 100);          /* must not crash */
    actor_table_set_lockout(&T, -1, true);                   /* must not crash */
}

/* M5b Task 8 fix round 1, finding 3: actor_table_remove() is the inverse of
 * add(), for a device whose wrapper no longer declares any action at all.
 * Everything about it goes: its actions, their guards, their spent budget
 * and its lockout -- which is exactly why it must never be a routine step
 * of re-binding (test_readd_preserves_guards_and_window() above pins the
 * other half of that rule). */
static void test_remove_undeclares_everything(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_set_lockout(&T, 3, true);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);

    assert(actor_table_remove(&T, 3));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 150)
           == ACTOR_REFUSED_UNKNOWN);
    /* Even a safety close, which is exempt from the guards but not from
     * "this device declares no such action". */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 150)
           == ACTOR_REFUSED_UNKNOWN);

    /* Removing something that was never declared reports so rather than
     * pretending, and a negative index is refused like everywhere else. */
    assert(!actor_table_remove(&T, 3));
    assert(!actor_table_remove(&T, -1));
}

/* The freed row is reused, so nothing of the removed device may survive
 * into the next one to occupy it: an inherited last_fire_s or window_count
 * would charge a DIFFERENT device's first command against a budget it
 * never spent -- and, with lockout, could leave a brand-new actuator
 * silently refusing everything. */
static void test_removed_row_is_reusable_and_clean(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 300, 0x01));
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_set_lockout(&T, 3, true);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 100);
    assert(actor_table_remove(&T, 3));

    assert(actor_table_add(&T, 7, ACT_IRRIGATION_OPEN, 300, 0x01));
    assert(actor_table_check(&T, 7, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 150) == ACTOR_OK);
}

/* And the capacity it frees is real: a fifth actuator was refused before
 * (test_capacity_refuses_fifth_actor), and must be accepted after a
 * removal rather than still counting against the table. */
static void test_remove_frees_capacity(void) {
    actor_table_init(&T);
    for (int d = 0; d < ACTOR_MAX_DEVICES; d++) {
        assert(actor_table_add(&T, d, ACT_SWITCH_ON, 0, 0));
    }
    assert(!actor_table_add(&T, 99, ACT_SWITCH_ON, 0, 0));
    assert(actor_table_remove(&T, 1));
    assert(actor_table_add(&T, 99, ACT_SWITCH_ON, 0, 0));
    assert(actor_table_check(&T, 99, ACT_SWITCH_ON, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
}

/* M5b Task 9: actor_table_action_flags() is the one thing pending_close
 * reads from this table -- whether ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF (bit
 * 0) is set for a declared pair, which decides whether the hub owes that
 * device a scheduled close at all. */
static void test_action_flags_read_back(void) {
    setup(); /* dev 3: ACT_IRRIGATION_OPEN flags=0x01, ACT_SWITCH_OFF flags=0x00 */
    uint8_t flags = 0xAA; /* poisoned, so a false "success" leaving it
                            * untouched would be caught */
    assert(actor_table_action_flags(&T, 3, ACT_IRRIGATION_OPEN, &flags));
    assert(flags == ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF);

    flags = 0xAA;
    assert(actor_table_action_flags(&T, 3, ACT_SWITCH_OFF, &flags));
    assert(flags == 0x00);
}

static void test_action_flags_undeclared_pair_or_device(void) {
    setup();
    uint8_t flags = 0;
    assert(!actor_table_action_flags(&T, 3, ACT_PUMP_RUN, &flags));   /* declared device, undeclared action */
    assert(!actor_table_action_flags(&T, 7, ACT_IRRIGATION_OPEN, &flags)); /* undeclared device */
    assert(!actor_table_action_flags(&T, -1, ACT_IRRIGATION_OPEN, &flags)); /* negative dev_idx */
}

int main(void) {
    test_bound_enforced(); test_wrapper_bound_tightens(); test_cooldown();
    test_rate_limit_fixed_window(); test_one_budget_across_sources(); test_lockout();
    test_capacity_refuses_fifth_actor(); test_unknown_device_or_action();
    test_bound_wins_over_lockout(); test_cooldown_wins_over_rate();
    test_zero_guards_stay_unlimited(); test_cooldown_before_first_fire_permits();
    test_window_count_saturates_not_wraps();
    test_readd_preserves_guards_and_window();
    test_safety_exempt_from_cooldown(); test_safety_exempt_from_rate();
    test_negative_dev_idx_rejected();
    test_remove_undeclares_everything();
    test_removed_row_is_reusable_and_clean();
    test_remove_frees_capacity();
    test_action_flags_read_back();
    test_action_flags_undeclared_pair_or_device();
    printf("test_actor_table: OK\n");
    return 0;
}
