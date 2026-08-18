/* tests/host/test_actor_queue.c -- the pure command queue (M5b Task 7,
 * spec sections 3 and 4.1). Exercises only actor_queue_t and its
 * actor_queue_* operations: TTL (a late command is dropped, never
 * executed late) and priority (a safety close jumps the queue). See
 * actor.h's own comment for why actor_request()/actor_service() -- which
 * touch alert_post() and the radio -- are NOT exercised here. */
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
    printf("test_actor_queue: OK\n");
    return 0;
}
