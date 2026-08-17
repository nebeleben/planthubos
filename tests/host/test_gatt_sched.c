#include <assert.h>
#include <stdio.h>
#include "gatt_sched.h"

/* Backoff doubles and then stops doubling -- an unreachable device must
 * not drift to an interval so long it never retries. */
static void test_backoff_doubles_and_caps(void)
{
    gatt_sched_reset();
    for (int i = 0; i < 10; i++) gatt_sched_fail(0, 1000);
    /* declared 600 s; effective interval capped at 8x = 4800 s */
    assert(gatt_sched_due(0, 600, 1000 + 4799) == false);
    assert(gatt_sched_due(0, 600, 1000 + 4800) == true);
}

/* One success clears the backoff completely. */
static void test_success_resets_backoff(void)
{
    gatt_sched_reset();
    gatt_sched_fail(0, 1000); gatt_sched_fail(0, 1000);
    gatt_sched_ok(0, 2000);
    assert(gatt_sched_fail_count(0) == 0);
    assert(gatt_sched_due(0, 600, 2000 + 600) == true);
}

/* Never-read device is due immediately; a device read a second ago is not. */
static void test_due_boundaries(void)
{
    gatt_sched_reset();
    assert(gatt_sched_due(3, 600, 12345) == true);   /* never contacted */

    gatt_sched_ok(3, 1000);
    assert(gatt_sched_due(3, 600, 1001) == false);    /* read a second ago */
    assert(gatt_sched_due(3, 600, 1000 + 599) == false);
    assert(gatt_sched_due(3, 600, 1000 + 600) == true);
}

/* Fix round 1: last_ok_s and last_attempt_s are separate fields. A
 * failure must never move last_ok_s (the Devices tab needs the TRUE last
 * successful read, even mid-backoff -- design spec section 5/8), but it
 * must move last_attempt_s (or backoff never anchors on the failure and
 * never actually backs off -- see gatt_sched_due()'s doc comment). This
 * is the whole point of fix round 1, so it gets its own test: success,
 * then several failures, checking last_ok_s holds still and due-ness is
 * governed by the failures' own timestamp, then a later success moving
 * last_ok_s forward again and clearing the backoff. */
static void test_last_ok_survives_failures_and_due_anchors_on_attempt(void)
{
    gatt_sched_reset();

    gatt_sched_ok(4, 1000);
    assert(gatt_sched_last_ok(4) == 1000);

    gatt_sched_fail(4, 1100);
    gatt_sched_fail(4, 1200);
    gatt_sched_fail(4, 1300);
    assert(gatt_sched_last_ok(4) == 1000);      /* unchanged by 3 failures */
    assert(gatt_sched_fail_count(4) == 3);

    /* declared interval 100 s; 3 fails caps the scale at 8x = 800 s,
     * anchored on the last ATTEMPT (1300), not the last success (1000).
     * Anchoring on 1000 instead would make this already overdue at 1300
     * (1300 - 1000 = 300 >= 100), defeating the backoff entirely. */
    assert(gatt_sched_due(4, 100, 1300 + 799) == false);
    assert(gatt_sched_due(4, 100, 1300 + 800) == true);

    gatt_sched_ok(4, 5000);
    assert(gatt_sched_last_ok(4) == 5000);      /* moved forward */
    assert(gatt_sched_fail_count(4) == 0);      /* backoff cleared */
    assert(gatt_sched_due(4, 100, 5000 + 100) == true);
}

/* A never-read device must report something a caller can tell apart from
 * "read at time zero" -- Task 7 renders "never" differently from "just
 * now". Backed by the internal explicit "has ever succeeded" flag (fix
 * round 2), not by last_ok_s being nonzero -- exercised here at the
 * smallest legitimate post-boot uptime, 1 second; test_ok_at_time_zero_
 * is_recorded_and_distinct_from_never below covers now_s == 0 itself. */
static void test_last_ok_never_is_distinct_from_time_zero(void)
{
    gatt_sched_reset();
    assert(gatt_sched_last_ok(5) == 0);
    assert(gatt_sched_fail_count(5) == 0);
    assert(gatt_sched_due(5, 600, 0) == true);   /* never attempted: due even at now_s=0 */

    gatt_sched_ok(5, 1);
    assert(gatt_sched_last_ok(5) == 1);
    assert(gatt_sched_last_ok(5) != 0);          /* distinguishable from "never" */
}

/* Fix round 2 (controller ruling): now_s == 0 is a real, reachable value
 * -- an immediate NimBLE rejection needs no radio round-trip, and uptime
 * is 1-second resolution, so the first second after boot is a real
 * window, not a theoretical one. Before this fix, gatt_sched_due()
 * inferred "never attempted" from last_attempt_s == 0, so a failure
 * landing exactly at t=0 was silently exempted from backoff -- this is
 * the reviewer's exact repro: a first contact that fails at t=0 must
 * still open a real backed-off window, not be treated as if nothing
 * happened. */
static void test_fail_at_time_zero_backs_off(void)
{
    gatt_sched_reset();
    gatt_sched_fail(6, 0);
    assert(gatt_sched_fail_count(6) == 1);

    /* declared interval 100 s; 1 fail -> scale 2x = 200 s window, anchored
     * on the attempt at t=0 -- NOT treated as "never attempted", which
     * would have short-circuited gatt_sched_due() to true here. */
    assert(gatt_sched_due(6, 100, 50) == false);
    assert(gatt_sched_due(6, 100, 199) == false);
    assert(gatt_sched_due(6, 100, 200) == true);
}

/* Same fix round 2 concern, success side (the reviewer flagged
 * gatt_sched_ok(dev, 0) as the same theoretical exposure): a success
 * landing exactly on now_s == 0 must be recorded as a real event and
 * stay distinguishable from a device that was never contacted at all.
 * The distinguishing behaviour is gatt_sched_due(): a never-contacted
 * device is due immediately even at now_s == 0, but once
 * gatt_sched_ok(dev, 0) has actually happened, due() stops short-
 * circuiting and instead honours the declared interval from that real
 * attempt, exactly as it would for any other attempt timestamp. */
static void test_ok_at_time_zero_is_recorded_and_distinct_from_never(void)
{
    gatt_sched_reset();
    assert(gatt_sched_due(7, 600, 0) == true);   /* never contacted: due even at t=0 */

    gatt_sched_ok(7, 0);
    assert(gatt_sched_fail_count(7) == 0);
    assert(gatt_sched_last_ok(7) == 0);          /* a real timestamp that happens to be 0 */

    assert(gatt_sched_due(7, 600, 0) == false);      /* no longer "never contacted" */
    assert(gatt_sched_due(7, 600, 599) == false);
    assert(gatt_sched_due(7, 600, 600) == true);
}

/* A never-contacted device is due immediately, at any now_s -- including
 * now_s == 0, which must not be mistaken for a past contact at t=0. */
static void test_never_contacted_due_immediately(void)
{
    gatt_sched_reset();
    assert(gatt_sched_due(8, 600, 0) == true);
    assert(gatt_sched_due(8, 600, 999999) == true);
    assert(gatt_sched_fail_count(8) == 0);
    assert(gatt_sched_last_ok(8) == 0);
}

/* Task 6 fix round 1: the third outcome -- the radio worked, the decode
 * emitted nothing. It must move the interval gate (or the hub reconnects on
 * every advertisement forever), clear the failure backoff (the radio is
 * demonstrably fine), and leave the last SUCCESSFUL read timestamp exactly
 * where it was (that timestamp plus last_error is the whole visibility
 * surface for a connect block that contributes nothing, now that the
 * event-log entry is cut from M5a). Each of those three is asserted
 * separately below, because getting any one of them from the wrong existing
 * function is precisely the mistake this outcome exists to prevent. */
static void test_attempt_moves_gate_without_faking_a_read(void)
{
    gatt_sched_reset();

    gatt_sched_ok(3, 1000);
    assert(gatt_sched_last_ok(3) == 1000);

    /* Two radio-fine/decode-empty attempts, well after the interval. */
    gatt_sched_attempt(3, 2000);
    assert(gatt_sched_last_ok(3) == 1000);   /* NOT advanced: this was not a read */
    assert(gatt_sched_fail_count(3) == 0);
    assert(gatt_sched_due(3, 600, 2599) == false);   /* gate DID move to 2000 */
    assert(gatt_sched_due(3, 600, 2600) == true);

    gatt_sched_attempt(3, 3000);
    assert(gatt_sched_last_ok(3) == 1000);   /* still frozen after a second one */

    /* And it clears an existing backoff rather than compounding it: three
     * failures put the device at the 8x cap, one radio-fine attempt returns
     * it to 1x. */
    gatt_sched_reset();
    gatt_sched_fail(4, 100);
    gatt_sched_fail(4, 200);
    gatt_sched_fail(4, 300);
    assert(gatt_sched_fail_count(4) == 3);
    assert(gatt_sched_due(4, 600, 300 + 4799) == false);   /* 8x600 = 4800 */
    gatt_sched_attempt(4, 400);
    assert(gatt_sched_fail_count(4) == 0);
    assert(gatt_sched_due(4, 600, 400 + 599) == false);
    assert(gatt_sched_due(4, 600, 400 + 600) == true);     /* back to 1x */
    assert(gatt_sched_last_ok(4) == 0);                    /* never succeeded, still never */

    /* A device that has ONLY ever had radio-fine/decode-empty attempts must
     * be indistinguishable from one that has never read: last_ok stays 0 and
     * "has ever succeeded" stays false. */
    gatt_sched_reset();
    gatt_sched_attempt(5, 0);
    assert(gatt_sched_last_ok(5) == 0);
    assert(gatt_sched_due(5, 600, 599) == false);   /* attempted at t=0, not "never" */
    assert(gatt_sched_due(5, 600, 600) == true);

    /* Out of range is a no-op, like every other entry point here. */
    gatt_sched_attempt(-1, 5);
    gatt_sched_attempt(GATT_SCHED_MAX_DEVICES, 5);
    gatt_sched_attempt(1000000, 5);
    assert(gatt_sched_fail_count(15) == 0);
    assert(gatt_sched_last_ok(15) == 0);
    assert(gatt_sched_due(15, 600, 0) == true);   /* untouched: still never attempted */
}

int main(void)
{
    test_backoff_doubles_and_caps();
    test_success_resets_backoff();
    test_due_boundaries();
    test_last_ok_survives_failures_and_due_anchors_on_attempt();
    test_last_ok_never_is_distinct_from_time_zero();
    test_fail_at_time_zero_backs_off();
    test_ok_at_time_zero_is_recorded_and_distinct_from_never();
    test_never_contacted_due_immediately();
    test_attempt_moves_gate_without_faking_a_read();

    printf("test_gatt_sched: OK\n");
    return 0;
}
