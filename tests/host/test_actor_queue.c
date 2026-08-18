/* tests/host/test_actor_queue.c -- the pure command queue and decision
 * logic (M5b Task 7, spec sections 3 and 4.1, plus fix round 1). Exercises
 * actor_queue_t/actor_queue_* (TTL and priority -- a late command is
 * dropped, never executed late; a safety close jumps the queue, and now
 * evicts to get in when the queue is full) and the pure decision
 * functions actor_request_decide()/actor_service_step() added in fix
 * round 1 to close the "checked at enqueue, never re-checked at dispatch"
 * gap. See actor.h's own comment for why actor_request()/actor_service()
 * themselves -- the thin ESP-IDF wrappers around these, which touch
 * alert_post() and the radio -- are NOT exercised here. */
#include "actor.h"
#include "action.h"
#include <assert.h>
#include <stdio.h>

/* A command whose deadline has passed is DROPPED, never executed late.
 * A watering command that sat behind a wedged connection for four minutes
 * is not a command any more, it is a hazard. */
static void test_expired_command_dropped(void) {
    actor_queue_t q; actor_queue_init(&q);
    assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .action_id = ACT_SWITCH_ON,
                                                .deadline_s = 100 }));
    actor_cmd_t out;
    assert(!actor_queue_pop(&q, &out, 101));
    assert(actor_queue_expired(&q) == 1);
}

static void test_deadline_boundary_is_inclusive(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 100 });
    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 100));
}

static void test_queue_full_refuses(void) {
    actor_queue_t q; actor_queue_init(&q);
    for (int i = 0; i < ACTOR_QUEUE_MAX; i++)
        assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 9999 }));
    assert(!actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 9999 }));
}

/* FIFO, except that a safety close jumps the queue: a pending close is the
 * one command whose lateness is a hazard rather than an inconvenience. */
static void test_safety_close_takes_priority(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .action_id = ACT_SWITCH_ON,
                                         .source = ACTOR_SRC_RULE, .deadline_s = 9999 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .action_id = ACT_SWITCH_OFF,
                                         .source = ACTOR_SRC_SAFETY, .deadline_s = 9999 });
    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 1));
    assert(out.source == ACTOR_SRC_SAFETY);
}

/* ---- Coverage beyond the brief's floor ---- */

/* Two safety closes: the priority rule only says a safety entry beats an
 * ordinary one, it does not say which safety entry wins when there are
 * several -- FIFO among ties is the only sane answer (the older close has
 * been waiting longer). Also proves the RULE command behind them is not
 * lost, just deferred to the third pop. */
static void test_two_safety_closes_resolve_fifo(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .action_id = ACT_SWITCH_ON,
                                         .source = ACTOR_SRC_RULE, .deadline_s = 9999 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 2, .action_id = ACT_SWITCH_OFF,
                                         .source = ACTOR_SRC_SAFETY, .deadline_s = 9999 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 3, .action_id = ACT_SWITCH_OFF,
                                         .source = ACTOR_SRC_SAFETY, .deadline_s = 9999 });
    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 1)); assert(out.source == ACTOR_SRC_SAFETY && out.dev_idx == 2);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.source == ACTOR_SRC_SAFETY && out.dev_idx == 3);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.source == ACTOR_SRC_RULE && out.dev_idx == 1);
}

static void test_pop_on_empty_queue_is_false_and_does_not_count_expired(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_cmd_t out;
    assert(!actor_queue_pop(&q, &out, 100));
    assert(actor_queue_expired(&q) == 0);
}

/* Several expired entries encountered in ONE pop() call must all be
 * dropped and all counted -- not just the first one found. */
static void test_multiple_expired_dropped_in_one_pop(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 10 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 2, .deadline_s = 20 });
    actor_cmd_t out;
    assert(!actor_queue_pop(&q, &out, 100));
    assert(actor_queue_expired(&q) == 2);
}

/* A valid command queued behind expired ones must still be found and
 * returned -- TTL drop must not stop the scan short. */
static void test_valid_command_behind_expired_ones_is_returned(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 10 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 2, .deadline_s = 200 });
    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 100));
    assert(out.dev_idx == 2);
    assert(actor_queue_expired(&q) == 1);
}

/* The same (device, action) pair queued twice is not deduplicated -- the
 * queue has no notion of identity, only capacity; actor_table's guards are
 * what bound repeated activation, not the queue. Both entries must survive
 * and come back out in FIFO order. */
static void test_duplicate_pair_not_deduplicated(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 5, .action_id = ACT_IRRIGATION_OPEN,
                                         .param = 10, .deadline_s = 9999 });
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 5, .action_id = ACT_IRRIGATION_OPEN,
                                         .param = 20, .deadline_s = 9999 });
    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 1)); assert(out.param == 10);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.param == 20);
}

/* actor_queue_expired() is cumulative across calls, not "since the last
 * pop" -- monitoring code polling it must see a running total. */
static void test_expired_counter_is_cumulative(void) {
    actor_queue_t q; actor_queue_init(&q);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 10 });
    actor_cmd_t out;
    actor_queue_pop(&q, &out, 100);
    assert(actor_queue_expired(&q) == 1);
    actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 2, .deadline_s = 10 });
    actor_queue_pop(&q, &out, 100);
    assert(actor_queue_expired(&q) == 2);
}

/* A full queue refuses a push even when every queued entry is already
 * expired -- push never inspects deadline_s, only actor_service()'s next
 * pop() reclaims the space. Documents the trade rather than hiding it. */
static void test_full_queue_of_expired_commands_still_refuses_push(void) {
    actor_queue_t q; actor_queue_init(&q);
    for (int i = 0; i < ACTOR_QUEUE_MAX; i++)
        assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 1 }));
    assert(!actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 1, .deadline_s = 9999 }));
}

/* ---- Fix round 1 (review): findings 1, 2 and 3 ---- */

/* Review finding 1 (CRITICAL): a safety close must be able to reach the
 * queue even when it is full of ordinary commands -- refusing it would
 * strand the actuator the close exists to shut. It evicts the OLDEST
 * non-safety entry, preserves FIFO order among the rest, and the new
 * safety entry still competes on priority at pop time like any other. */
static void test_safety_push_evicts_oldest_ordinary_when_full(void) {
    actor_queue_t q; actor_queue_init(&q);
    for (int i = 0; i < ACTOR_QUEUE_MAX; i++)
        assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = (int8_t)i,
                                                     .source = ACTOR_SRC_RULE, .deadline_s = 9999 }));
    assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 9, .source = ACTOR_SRC_SAFETY,
                                                .deadline_s = 9999 }));
    assert(q.last_evicted_valid);
    assert(q.last_evicted.dev_idx == 0); /* the oldest (first-pushed) entry */

    actor_cmd_t out;
    assert(actor_queue_pop(&q, &out, 1)); assert(out.source == ACTOR_SRC_SAFETY && out.dev_idx == 9);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.dev_idx == 1);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.dev_idx == 2);
    assert(actor_queue_pop(&q, &out, 1)); assert(out.dev_idx == 3);
}

/* A safety push is refused ONLY when every queued entry is itself a
 * safety command -- a genuine overload with nothing left to evict. */
static void test_safety_push_refused_only_when_all_queued_are_safety(void) {
    actor_queue_t q; actor_queue_init(&q);
    for (int i = 0; i < ACTOR_QUEUE_MAX; i++)
        assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = (int8_t)i,
                                                     .source = ACTOR_SRC_SAFETY, .deadline_s = 9999 }));
    assert(!actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 9, .source = ACTOR_SRC_SAFETY,
                                                 .deadline_s = 9999 }));
    assert(!q.last_evicted_valid);
}

/* last_evicted_valid is one-shot, reset at the start of every push -- a
 * caller checking it after an ORDINARY push (that neither evicted nor
 * needed to) must not see a stale true from an earlier eviction. */
static void test_eviction_flag_is_one_shot(void) {
    actor_queue_t q; actor_queue_init(&q);
    for (int i = 0; i < ACTOR_QUEUE_MAX; i++)
        assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = (int8_t)i,
                                                     .source = ACTOR_SRC_RULE, .deadline_s = 9999 }));
    assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 9, .source = ACTOR_SRC_SAFETY,
                                                .deadline_s = 9999 }));
    assert(q.last_evicted_valid);

    actor_cmd_t out;
    actor_queue_pop(&q, &out, 1); /* makes room again */
    assert(actor_queue_push(&q, &(actor_cmd_t){ .dev_idx = 8, .source = ACTOR_SRC_RULE,
                                                .deadline_s = 9999 }));
    assert(!q.last_evicted_valid);
}

/* Review finding 2 (CRITICAL): two commands for the same rate-capped pair
 * can both pass actor_request_decide() before either is serviced (they
 * "raced" ahead of any recording) -- the enqueue-time check alone cannot
 * see that. actor_service_step()'s RE-check at dispatch time is what
 * catches it: the first dispatch records the fire, and the second
 * dispatch's re-check now sees the spent budget and drops the command
 * instead of sending it. */
static void test_dispatch_recheck_catches_rate_cap_raced_at_enqueue(void) {
    actor_table_t t; actor_table_init(&t);
    assert(actor_table_add(&t, 3, ACT_IRRIGATION_OPEN, 300, 0));
    actor_table_set_guards(&t, 3, ACT_IRRIGATION_OPEN, /*cooldown_s*/ 0, /*max_per_hour*/ 1);
    actor_queue_t q; actor_queue_init(&q);

    actor_request_result_t r1 = actor_request_decide(&t, &q, 3, ACT_IRRIGATION_OPEN, 10,
                                                       ACTOR_SRC_RULE, 9999, 100);
    assert(r1.verdict == ACTOR_OK && r1.queued);
    actor_request_result_t r2 = actor_request_decide(&t, &q, 3, ACT_IRRIGATION_OPEN, 10,
                                                       ACTOR_SRC_MANUAL, 9999, 101);
    assert(r2.verdict == ACTOR_OK && r2.queued); /* neither has recorded yet */

    actor_service_result_t s1 = actor_service_step(&t, &q, 102);
    assert(s1.dispatched && !s1.redecline);

    actor_service_result_t s2 = actor_service_step(&t, &q, 103);
    assert(!s2.dispatched);
    assert(s2.redecline && s2.redecline_verdict == ACTOR_REFUSED_RATE);
}

/* Same finding: an operator setting lockout AFTER a rule's command is
 * already queued must still stop it -- "the operator pressed stop" has to
 * reach a command that is merely waiting for the radio, not just the next
 * one requested. */
static void test_dispatch_recheck_catches_lockout_set_after_enqueue(void) {
    actor_table_t t; actor_table_init(&t);
    assert(actor_table_add(&t, 3, ACT_IRRIGATION_OPEN, 300, 0));
    actor_queue_t q; actor_queue_init(&q);

    actor_request_result_t r = actor_request_decide(&t, &q, 3, ACT_IRRIGATION_OPEN, 10,
                                                      ACTOR_SRC_RULE, 9999, 100);
    assert(r.verdict == ACTOR_OK && r.queued);

    actor_table_set_lockout(&t, 3, true); /* operator hits stop before dispatch */

    actor_service_result_t s = actor_service_step(&t, &q, 101);
    assert(!s.dispatched);
    assert(s.redecline && s.redecline_verdict == ACTOR_REFUSED_LOCKOUT);
}

/* The re-check must not fire on an otherwise-healthy dispatch: a queued
 * command whose guard state hasn't changed still dispatches and records. */
static void test_dispatch_recheck_permits_unchanged_guard_state(void) {
    actor_table_t t; actor_table_init(&t);
    assert(actor_table_add(&t, 3, ACT_IRRIGATION_OPEN, 300, 0));
    actor_queue_t q; actor_queue_init(&q);

    actor_request_result_t r = actor_request_decide(&t, &q, 3, ACT_IRRIGATION_OPEN, 10,
                                                      ACTOR_SRC_RULE, 9999, 100);
    assert(r.verdict == ACTOR_OK && r.queued);

    actor_service_result_t s = actor_service_step(&t, &q, 101);
    assert(s.dispatched && !s.redecline);
    assert(s.cmd.dev_idx == 3 && s.cmd.param == 10);
}

int main(void) {
    test_expired_command_dropped();
    test_deadline_boundary_is_inclusive();
    test_queue_full_refuses();
    test_safety_close_takes_priority();
    test_two_safety_closes_resolve_fifo();
    test_pop_on_empty_queue_is_false_and_does_not_count_expired();
    test_multiple_expired_dropped_in_one_pop();
    test_valid_command_behind_expired_ones_is_returned();
    test_duplicate_pair_not_deduplicated();
    test_expired_counter_is_cumulative();
    test_full_queue_of_expired_commands_still_refuses_push();
    test_safety_push_evicts_oldest_ordinary_when_full();
    test_safety_push_refused_only_when_all_queued_are_safety();
    test_eviction_flag_is_one_shot();
    test_dispatch_recheck_catches_rate_cap_raced_at_enqueue();
    test_dispatch_recheck_catches_lockout_set_after_enqueue();
    test_dispatch_recheck_permits_unchanged_guard_state();
    printf("test_actor_queue: OK\n");
    return 0;
}
