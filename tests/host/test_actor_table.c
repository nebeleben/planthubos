/* tests/host/test_actor_table.c */
#include "actor_table.h"
#include <assert.h>
#include <stdio.h>

static actor_table_t T;
static void setup(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    /* The close too: test_lockout() asserts a safety close is permitted, and
     * an action the device does not declare would refuse as UNKNOWN long
     * before the lockout rule was ever consulted. */
    assert(actor_table_add(&T, 3, ACT_SWITCH_OFF, 1, 0, 0x00));
}

static void test_bound_enforced(void) {
    setup();
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 301, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

/* A wrapper that declared a LOWER max than the firmware's is the effective
 * bound -- tightening is always allowed. Asserts BOTH sides: at the
 * tightened max it must still be OK (an off-by-one that tightened too far
 * would pass a refusal-only test), and just past it must be refused. */
static void test_wrapper_bound_tightens(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 60, 0x01));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 60, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 61, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

static void test_cooldown(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 60, /*max_per_hour*/ 10);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 159) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 160) == ACTOR_OK);
}

/* Fixed hourly window (spec section 4.2). The boundary behaviour is a
 * documented trade, so it is asserted rather than left to chance. Also
 * asserts the FIRST fire is permitted (a too-strict off-by-one like
 * `count >= max_per_hour - 1` would still fail the refusal-only checks
 * below but pass a suite that never checked the positive case). */
static void test_rate_limit_fixed_window(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, 0, 2);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 200);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_RATE);
    /* New window opens 3600 s after the window START, not after the last fire. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 3700) == ACTOR_OK);
}

/* One budget per (device, action), whatever asked -- that is the whole
 * reason guards attach here and not to the plant or the rule. */
static void test_one_budget_across_sources(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, 0, 2);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);   /* a rule */
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 110);   /* a manual press */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_MANUAL, 120) == ACTOR_REFUSED_RATE);
}

/* Lockout is the operator's stop button: it refuses rules, permits manual,
 * and permits the safety close -- a lockout that blocked the close would
 * strand an actuator open. */
static void test_lockout(void) {
    setup();
    actor_table_set_lockout(&T, 3, true);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_LOCKOUT);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_MANUAL, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 100) == ACTOR_OK);
}

static void test_capacity_refuses_fifth_actor(void) {
    actor_table_init(&T);
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) assert(actor_table_add(&T, i, ACT_SWITCH_ON, 1, 0, 0));
    assert(!actor_table_add(&T, 99, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_full_drops(&T) == 1);
}

static void test_unknown_device_or_action(void) {
    setup();
    assert(actor_table_check(&T, 7, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, 3, ACT_PUMP_RUN, 1, 10, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    /* ACT_SWITCH_OFF IS declared by this device (see setup), so it must not
     * be refused as unknown -- this pins the distinction. */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
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
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 301, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_BOUND);
}

/* Ordering: COOLDOWN must win over RATE when both would refuse. */
static void test_cooldown_wins_over_rate(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    /* At t=150: still within the 600 s cooldown AND already at the 1/hour
     * cap -- must report COOLDOWN, per the fixed unknown->bound->lockout->
     * cooldown->rate order. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_COOLDOWN);
}

/* Zero means unlimited/off for BOTH guards, and that must hold even after
 * many recorded fires, not just when neither guard is configured. */
static void test_zero_guards_stay_unlimited(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 0, /*max_per_hour*/ 0);
    for (uint32_t t = 0; t < 50; t++)
        actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100 + t);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 149) == ACTOR_OK);
}

/* A cooldown configured before the pair has ever fired must not refuse:
 * window_count == 0 (never recorded) must not be confused with a fire at
 * last_fire_s == 0. */
static void test_cooldown_before_first_fire_permits(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 3600, /*max_per_hour*/ 0);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 0) == ACTOR_OK);
}

/* window_count is a uint8_t: it must saturate at 255 rather than wrap to 0
 * and silently reopen "capacity" mid-window. Uses a cooldown of 0 so only
 * the rate axis is exercised, and a max_per_hour above the saturation
 * point so the refusal, once reached, must never flip back to OK within
 * the same window. */
static void test_window_count_saturates_not_wraps(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, 0, 255);
    for (int i = 0; i < 300; i++)
        actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100 + (uint32_t)i);
    /* Still inside the same 3600 s window as the first record at t=100. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 399) == ACTOR_REFUSED_RATE);
}

/* Review round 1, finding 1 (CRITICAL): re-declaring an already-tracked
 * pair -- a wrapper re-parse, a re-discovery pass, an API-driven wrapper
 * update -- must not erase an hourly budget already spent or reset the
 * operator's cooldown. Traces the reviewer's own repro: a cooldown earned
 * by two recorded fires must still be in force after an identical re-add. */
static void test_readd_preserves_guards_and_window(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 2);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 200);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_COOLDOWN);
    /* Re-declare the identical pair. */
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 300) == ACTOR_REFUSED_COOLDOWN);
}

/* Review round 1, finding 2 (Important): ACTOR_SRC_SAFETY is exempt from
 * cooldown -- a close-retry storm (spec section 4.3/4.5) must not be
 * refused by a guard that exists to rate-shape rules and manual presses,
 * not to block the one source whose job is closing something already
 * open. */
static void test_safety_exempt_from_cooldown(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_SWITCH_OFF, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 0);
    actor_table_record(&T, 3, ACT_SWITCH_OFF, 1, 100);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 150) == ACTOR_OK);
}

/* Same finding, the rate axis: a user-configured hourly cap on switch.off
 * must not be able to strand an actuator open either. */
static void test_safety_exempt_from_rate(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_SWITCH_OFF, 1, /*cooldown_s*/ 0, /*max_per_hour*/ 1);
    actor_table_record(&T, 3, ACT_SWITCH_OFF, 1, 100);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_RATE);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 150) == ACTOR_OK);
}

/* M5b whole-branch review, finding 6 (deferred minor from Task 6, promoted
 * to must-fix): the OTHER half of the source table -- SAFETY is exempt from
 * the three RATE-SHAPING guards (the two tests above) and is emphatically
 * NOT exempt from the two correctness ones. That distinction only holds
 * because unknown and bound are evaluated BEFORE actor_table_check()'s
 * `if (source == ACTOR_SRC_SAFETY) return ACTOR_OK` short-circuit; move
 * that line up by two checks and the parameter bound silently stops
 * binding on a safety close, with nothing failing. This pins the ordering
 * so that reorder is a test failure instead.
 *
 * BOUND on a safety source is reachable in practice: pending_close arms
 * ACT_SWITCH_OFF, and a hand-posted wrapper (or a future close carrying a
 * parameter) reaching this door with an out-of-range value must be refused
 * exactly as a rule would be -- it is a correctness check, not a rate
 * limit. */
static void test_bound_and_unknown_still_refuse_safety(void) {
    setup();

    /* UNKNOWN: a device that declares nothing, and a declared device asked
     * for an action it never declared. Neither becomes commandable just
     * because the caller is the safety core. */
    assert(actor_table_check(&T, 9, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, 3, ACT_PUMP_RUN, 1, 0, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, -1, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_UNKNOWN);

    /* BOUND: over the effective ceiling, and non-zero on a parameterless
     * action (action_param_ok()'s own rule) -- both refuse SAFETY. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 301, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_BOUND);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 1, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_BOUND);

    /* And the real close -- parameterless, param 0 -- still passes, so the
     * assertions above are about the bound and not about SAFETY being
     * refused generally. */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 100) == ACTOR_OK);

    /* The ordering itself: with a lockout set AND an out-of-range
     * parameter, a safety close reports BOUND -- proving bound is
     * evaluated before the SAFETY short-circuit, not merely that it is
     * evaluated at all. */
    actor_table_set_lockout(&T, 3, true);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 301, ACTOR_SRC_SAFETY, 100)
           == ACTOR_REFUSED_BOUND);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 100) == ACTOR_OK);
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
    assert(!actor_table_add(&T, -1, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_full_drops(&T) == 0);
    assert(actor_table_check(&T, -1, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    assert(!actor_table_set_guards(&T, -1, ACT_SWITCH_ON, 1, 10, 1));
    actor_table_record(&T, -1, ACT_SWITCH_ON, 1, 100);          /* must not crash */
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
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_set_lockout(&T, 3, true);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);

    assert(actor_table_remove(&T, 3));
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 150)
           == ACTOR_REFUSED_UNKNOWN);
    /* Even a safety close, which is exempt from the guards but not from
     * "this device declares no such action". */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_SAFETY, 150)
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
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 1);
    actor_table_set_lockout(&T, 3, true);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    assert(actor_table_remove(&T, 3));

    assert(actor_table_add(&T, 7, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    assert(actor_table_check(&T, 7, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 150) == ACTOR_OK);
}

/* And the capacity it frees is real: a fifth actuator was refused before
 * (test_capacity_refuses_fifth_actor), and must be accepted after a
 * removal rather than still counting against the table. */
static void test_remove_frees_capacity(void) {
    actor_table_init(&T);
    for (int d = 0; d < ACTOR_MAX_DEVICES; d++) {
        assert(actor_table_add(&T, d, ACT_SWITCH_ON, 1, 0, 0));
    }
    assert(!actor_table_add(&T, 99, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_remove(&T, 1));
    assert(actor_table_add(&T, 99, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_check(&T, 99, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
}

/* M5b Task 9: actor_table_action_flags() is the one thing pending_close
 * reads from this table -- whether ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF (bit
 * 0) is set for a declared pair, which decides whether the hub owes that
 * device a scheduled close at all. */
static void test_action_flags_read_back(void) {
    setup(); /* dev 3: ACT_IRRIGATION_OPEN flags=0x01, ACT_SWITCH_OFF flags=0x00 */
    uint8_t flags = 0xAA; /* poisoned, so a false "success" leaving it
                            * untouched would be caught */
    assert(actor_table_action_flags(&T, 3, ACT_IRRIGATION_OPEN, 1, &flags));
    assert(flags == ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF);

    flags = 0xAA;
    assert(actor_table_action_flags(&T, 3, ACT_SWITCH_OFF, 1, &flags));
    assert(flags == 0x00);
}

static void test_action_flags_undeclared_pair_or_device(void) {
    setup();
    uint8_t flags = 0;
    assert(!actor_table_action_flags(&T, 3, ACT_PUMP_RUN, 1, &flags));   /* declared device, undeclared action */
    assert(!actor_table_action_flags(&T, 7, ACT_IRRIGATION_OPEN, 1, &flags)); /* undeclared device */
    assert(!actor_table_action_flags(&T, -1, ACT_IRRIGATION_OPEN, 1, &flags)); /* negative dev_idx */
}

/* ---- M5b Task 11: actor_table_pair_state()/actor_table_lockout() ---- */

/* Fresh pair: never fired, no guards configured, MANUAL would succeed
 * right now. Also pins param_max as the EFFECTIVE (wrapper-tightened)
 * bound, not action.h's raw 300 ceiling. */
static void test_pair_state_fresh_pair(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 60, 0x01)); /* firmware max is 300 */
    actor_pair_state_t s;
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 100, &s));
    assert(s.param_max == 60);
    assert(s.cooldown_s == 0);
    assert(s.max_per_hour == 0);
    assert(s.activations_this_hour == 0);
    assert(!s.has_fired);
    assert(s.live_verdict == ACTOR_OK);
}

/* Undeclared pair, undeclared device, negative dev_idx: all false, same
 * not-declared contract as actor_table_action_flags(). */
static void test_pair_state_undeclared(void) {
    setup();
    actor_pair_state_t s;
    assert(!actor_table_pair_state(&T, 3, ACT_PUMP_RUN, 1, 100, &s));
    assert(!actor_table_pair_state(&T, 7, ACT_IRRIGATION_OPEN, 1, 100, &s));
    assert(!actor_table_pair_state(&T, -1, ACT_IRRIGATION_OPEN, 1, 100, &s));
}

/* After a fire: has_fired/last_fire_s report it, activations_this_hour
 * counts it within the window, and a configured cooldown shows up as the
 * live verdict -- exactly matching what actor_table_check() itself would
 * say for a MANUAL request right now. */
static void test_pair_state_after_fire_cooldown(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown_s*/ 600, /*max_per_hour*/ 10);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    actor_pair_state_t s;
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 150, &s));
    assert(s.has_fired);
    assert(s.last_fire_s == 100);
    assert(s.activations_this_hour == 1);
    assert(s.live_verdict == ACTOR_REFUSED_COOLDOWN);
    /* Matches actor_table_check() itself at the same instant. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_MANUAL, 150) == ACTOR_REFUSED_COOLDOWN);

    /* Past the cooldown: OK again, and matches actor_table_check(). */
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 700, &s));
    assert(s.live_verdict == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_MANUAL, 700) == ACTOR_OK);
}

/* Rate cap reached: live_verdict reports RATE, and activations_this_hour
 * reads back the count actually spent -- then a new window resets both. */
static void test_pair_state_rate_and_window_reset(void) {
    setup();
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, 0, /*max_per_hour*/ 2);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 200);
    actor_pair_state_t s;
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, &s));
    assert(s.activations_this_hour == 2);
    assert(s.live_verdict == ACTOR_REFUSED_RATE);

    /* A new window (3600 s after the window START) reports 0 spent again,
     * even though window_count itself is not reset until the next
     * actor_table_record() -- activations_this_hour must reflect "is the
     * window still open", not the raw stored counter. */
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 3700, &s));
    assert(s.activations_this_hour == 0);
    assert(s.live_verdict == ACTOR_OK);
}

/* Lockout refuses RULE but never MANUAL (actor_table_check()'s own module
 * contract), so live_verdict must stay ACTOR_OK under lockout -- the
 * JSON's separate "lockout" boolean is what reports the stop button, not
 * this field. */
static void test_pair_state_lockout_does_not_appear_as_live_verdict(void) {
    setup();
    actor_table_set_lockout(&T, 3, true);
    actor_pair_state_t s;
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 1, 100, &s));
    assert(s.live_verdict == ACTOR_OK);
}

static void test_lockout_read_back(void) {
    setup();
    bool on = true;
    assert(actor_table_lockout(&T, 3, &on));
    assert(!on);

    actor_table_set_lockout(&T, 3, true);
    on = false;
    assert(actor_table_lockout(&T, 3, &on));
    assert(on);
}

static void test_lockout_undeclared(void) {
    setup();
    bool on;
    assert(!actor_table_lockout(&T, 7, &on));   /* undeclared device */
    assert(!actor_table_lockout(&T, -1, &on));  /* negative dev_idx */
}

/* Reconcile/prune: a device announce carries the device's full current
 * action set, so an action previously declared but absent from the latest
 * announce must be dropped -- while a surviving action keeps its guards and
 * spent budget (the whole reason actor_table_add re-declares in place).
 * This is the fix for a runtime-reclassified Zigbee knob that shed its
 * phantom switch.on/off actions after interviewing as a switch. */
static void test_prune_absent_drops_and_keeps_guards(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 1, 300, 0x01));
    assert(actor_table_add(&T, 3, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_add(&T, 3, ACT_SWITCH_OFF, 1, 0, 0));
    actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 1, /*cooldown*/ 60, /*rate*/ 10);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1, 100);

    const uint8_t keep[] = { ACT_IRRIGATION_OPEN };
    assert(actor_table_prune_absent(&T, 3, keep, 1));   /* something removed */

    /* The two switch actions are gone... */
    assert(actor_table_check(&T, 3, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 200) == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 200) == ACTOR_REFUSED_UNKNOWN);
    /* ...the survivor is still declared AND still inside its cooldown: guard
     * and spent budget preserved, not reset to a fresh slot (a reset would
     * read as OK at 150). */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 150) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 1, 10, ACTOR_SRC_RULE, 160) == ACTOR_OK);
}

/* Pruning to an empty set frees the whole device row (like a remove): its
 * actions become UNKNOWN and its capacity is reclaimed. */
static void test_prune_to_empty_frees_row(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 5, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_add(&T, 5, ACT_SWITCH_OFF, 1, 0, 0));
    assert(actor_table_prune_absent(&T, 5, NULL, 0));   /* everything removed */
    assert(actor_table_check(&T, 5, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    assert(actor_table_check(&T, 5, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    /* Capacity reclaimed: every row free again, so a full table's worth of
     * fresh devices all fit (the last would be refused if row 5 lingered). */
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) assert(actor_table_add(&T, 100 + i, ACT_SWITCH_ON, 1, 0, 0));
}

/* No-op when the announce still lists every declared action: nothing
 * removed (returns false), everything stays declared. */
static void test_prune_absent_noop_when_all_present(void) {
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_add(&T, 3, ACT_SWITCH_OFF, 1, 0, 0));
    const uint8_t keep[] = { ACT_SWITCH_ON, ACT_SWITCH_OFF };
    assert(!actor_table_prune_absent(&T, 3, keep, 2));  /* nothing to remove */
    assert(actor_table_check(&T, 3, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
}

/* A negative dev_idx and an undeclared device both prune nothing. */
static void test_prune_absent_negative_or_unknown(void) {
    actor_table_init(&T);
    const uint8_t one[] = { ACT_SWITCH_ON };
    assert(!actor_table_prune_absent(&T, -1, one, 1));
    assert(!actor_table_prune_absent(&T, 7, one, 1));   /* no such row */
}

/* ---- M8 Task 5: (dev_idx, action_id, endpoint) key (multi-endpoint) ---- */

/* --- two switch.on actions on endpoints 1 and 2 are independently
 * declarable, requestable and guarded. --- */
static void test_multi_endpoint_actions_are_independent(void) {
    actor_table_t E; actor_table_init(&E);
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 1, 0, 0));   /* dev 5, ep1 */
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 2, 0, 0));   /* dev 5, ep2 -- distinct slot */
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 2, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    actor_table_set_guards(&E, 5, ACT_SWITCH_ON, 1, /*cooldown*/ 60, /*max*/ 1);
    actor_table_record(&E, 5, ACT_SWITCH_ON, 1, 100);
    /* ep1 now cooling down, ep2 is untouched */
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 120) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 2, 0, ACTOR_SRC_RULE, 120) == ACTOR_OK);
}

/* Beyond the brief's floor: ACTOR_MAX_ACTIONS is now 8, sized for a 4-gang
 * device (4 gangs x switch.on/switch.off). All 8 (action, endpoint)
 * instances on ONE dev_idx must fit, and a 9th is refused exactly like the
 * pre-existing per-device capacity test, just at the new ceiling. */
static void test_four_gang_device_fits_and_a_ninth_instance_is_refused(void) {
    actor_table_t E; actor_table_init(&E);
    for (uint8_t ep = 1; ep <= 4; ep++) {
        assert(actor_table_add(&E, 5, ACT_SWITCH_ON, ep, 0, 0));
        assert(actor_table_add(&E, 5, ACT_SWITCH_OFF, ep, 0, 0));
    }
    for (uint8_t ep = 1; ep <= 4; ep++) {
        assert(actor_table_check(&E, 5, ACT_SWITCH_ON, ep, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
        assert(actor_table_check(&E, 5, ACT_SWITCH_OFF, ep, 0, ACTOR_SRC_SAFETY, 100) == ACTOR_OK);
    }
    /* A 9th instance on the same device -- e.g. a 5th gang's switch.on --
     * finds no free slot: refused, and counted as a genuinely full row. */
    assert(!actor_table_add(&E, 5, ACT_SWITCH_ON, 5, 0, 0));
    assert(actor_table_full_drops(&E) == 1);
}

/* ---- Whole-branch review, F1 fix: endpoint 0 ("unspecified") resolves to
 * the lowest declared endpoint on the actuate path, mirroring the read
 * path's own treatment of endpoint 0 (registry_get_cap_ep). ---- */

/* (a) A single-gang actuator declared ONLY at endpoint 2 (no endpoint-1
 * slot at all) -- exactly the regression this fix closes: a bare rule ref
 * or an endpoint-less manual POST used to force endpoint 1 and be refused
 * ACTOR_REFUSED_UNKNOWN even though the device is perfectly well declared,
 * just not at 1. An endpoint-0 request must resolve to endpoint 2, be
 * accepted, and actually land its record on the ep2 slot. */
static void test_endpoint_zero_resolves_to_sole_non_one_endpoint(void) {
    actor_table_t E; actor_table_init(&E);
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 2, 0, 0));   /* ep2 only */
    assert(actor_table_resolve_endpoint(&E, 5, ACT_SWITCH_ON, 0) == 2);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    actor_table_set_guards(&E, 5, ACT_SWITCH_ON, 2, /*cooldown*/ 60, /*max*/ 0);
    actor_table_record(&E, 5, ACT_SWITCH_ON, 0, 100);
    /* The record above must have landed on the ep2 slot -- verified by
     * reading its cooldown back, both via endpoint 0 (re-resolves to the
     * same slot) and via the explicit endpoint. */
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 120) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 2, 0, ACTOR_SRC_RULE, 120) == ACTOR_REFUSED_COOLDOWN);
}

/* (b) Both endpoints 1 and 2 declared: endpoint 0 resolves to the LOWEST
 * (1), an explicit 2 still hits the ep2 slot, and the two stay
 * independently guarded -- resolving 0 must never collapse per-endpoint
 * guard state. */
static void test_endpoint_zero_resolves_to_lowest_when_multiple_declared(void) {
    actor_table_t E; actor_table_init(&E);
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 1, 0, 0));
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 2, 0, 0));
    assert(actor_table_resolve_endpoint(&E, 5, ACT_SWITCH_ON, 0) == 1);

    actor_table_set_guards(&E, 5, ACT_SWITCH_ON, 1, /*cooldown*/ 60, /*max*/ 0);
    /* A 0-endpoint request/record must land on ep1 (the lowest), putting
     * ep1 into cooldown while leaving ep2 untouched. */
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 100) == ACTOR_OK);
    actor_table_record(&E, 5, ACT_SWITCH_ON, 0, 100);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 120) == ACTOR_REFUSED_COOLDOWN);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 1, 0, ACTOR_SRC_RULE, 120) == ACTOR_REFUSED_COOLDOWN);
    /* ep2, named explicitly, is a completely independent slot: no cooldown
     * configured there, so it stays OK throughout -- guard independence
     * survives endpoint-0 resolution. */
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 2, 0, ACTOR_SRC_RULE, 120) == ACTOR_OK);
}

/* (c) An action declared at no endpoint at all (device 5 declares only
 * ACT_SWITCH_OFF, never ACT_SWITCH_ON) -- an endpoint-0 request for the
 * undeclared action must still refuse ACTOR_REFUSED_UNKNOWN, exactly as
 * before this fix: there is no "lowest endpoint" to fall back to when the
 * action itself was never declared anywhere, on this device or at all. */
static void test_endpoint_zero_stays_unknown_when_action_never_declared(void) {
    actor_table_t E; actor_table_init(&E);
    assert(actor_table_add(&E, 5, ACT_SWITCH_OFF, 1, 0, 0));
    assert(actor_table_resolve_endpoint(&E, 5, ACT_SWITCH_ON, 0) == 0);
    assert(actor_table_check(&E, 5, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
    /* Also true for a dev_idx with no declared row at all. */
    assert(actor_table_resolve_endpoint(&E, 9, ACT_SWITCH_ON, 0) == 0);
    assert(actor_table_check(&E, 9, ACT_SWITCH_ON, 0, 0, ACTOR_SRC_RULE, 100) == ACTOR_REFUSED_UNKNOWN);
}

/* A real (non-zero) endpoint must pass through actor_table_resolve_endpoint()
 * completely unchanged, whether or not that exact endpoint is declared --
 * this function only ever substitutes for the 0 sentinel, never remaps a
 * caller's explicit choice. */
static void test_resolve_endpoint_passes_through_nonzero_unchanged(void) {
    actor_table_t E; actor_table_init(&E);
    assert(actor_table_add(&E, 5, ACT_SWITCH_ON, 2, 0, 0));
    assert(actor_table_resolve_endpoint(&E, 5, ACT_SWITCH_ON, 2) == 2);
    assert(actor_table_resolve_endpoint(&E, 5, ACT_SWITCH_ON, 7) == 7);   /* not even declared */
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
    test_bound_and_unknown_still_refuse_safety();
    test_negative_dev_idx_rejected();
    test_remove_undeclares_everything();
    test_removed_row_is_reusable_and_clean();
    test_remove_frees_capacity();
    test_action_flags_read_back();
    test_action_flags_undeclared_pair_or_device();
    test_pair_state_fresh_pair();
    test_pair_state_undeclared();
    test_pair_state_after_fire_cooldown();
    test_pair_state_rate_and_window_reset();
    test_pair_state_lockout_does_not_appear_as_live_verdict();
    test_lockout_read_back();
    test_lockout_undeclared();
    test_prune_absent_drops_and_keeps_guards();
    test_prune_to_empty_frees_row();
    test_prune_absent_noop_when_all_present();
    test_prune_absent_negative_or_unknown();
    test_multi_endpoint_actions_are_independent();
    test_four_gang_device_fits_and_a_ninth_instance_is_refused();
    test_endpoint_zero_resolves_to_sole_non_one_endpoint();
    test_endpoint_zero_resolves_to_lowest_when_multiple_declared();
    test_endpoint_zero_stays_unknown_when_action_never_declared();
    test_resolve_endpoint_passes_through_nonzero_unchanged();
    printf("test_actor_table: OK\n");
    return 0;
}
