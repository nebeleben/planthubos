#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "actor_table.h"

/* actor_request(), the command queue and its TTL (M5b Task 7, design spec
 * section 3 and section 4.1). actor_request() is the ONLY door onto an
 * actuator: a rule (Task 10), a manual button press (Task 11) and a
 * scheduled close (Task 9) all call this one function, so a guard added
 * here (Task 6's actor_table_check()) binds every source with no way
 * around it. If a future task ever wants a second way to reach the radio,
 * that is the design going wrong, not a shortcut worth taking.
 *
 * The queue (actor_queue_t and its actor_queue_* operations, below) is
 * pure C99, no ESP-IDF, so tests/host/test_actor_queue.c can prove its TTL
 * and priority rules by direct execution on the host -- the same split
 * this component already uses between actor_table.c (pure) and alert.c
 * (the FreeRTOS-touching front door).
 *
 * Fix round 1 (review) pushed that split one level deeper: the DECISION
 * logic of actor_request()/actor_service() -- "check now, decide, act on
 * the queue" -- is also pure (actor_request_decide()/actor_service_step()
 * below), taking the table, the queue and `now_s` as plain arguments and
 * returning a result struct instead of touching a lock, a clock or
 * alert_post(). actor_request()/actor_service() themselves shrink to: take
 * the lock, read the clock, call the pure function, release the lock, then
 * turn the result into alert_post() calls and a dispatch-hook call outside
 * the lock. Every ESP-IDF-only line (the mutex, esp_timer_get_time(), the
 * alert_post() call sites, the dispatch hand-off) is `#ifdef ESP_PLATFORM`-
 * gated exactly the way unknown_capture.c already gates its own mutex, so
 * this whole file -- not just the queue struct -- compiles and links with
 * plain `cc` for the host test, with no FreeRTOS/ESP-IDF toolchain
 * required, AND the two decision functions that matter most for safety are
 * now directly host-testable. */

#define ACTOR_QUEUE_MAX 4

typedef struct {
    int8_t   dev_idx;
    uint8_t  action_id;
    uint8_t  source;       /* actor_source_t, see actor_table.h */
    uint16_t param;
    uint32_t deadline_s;   /* absolute actor_now_s()-scale seconds -- see
                             * actor_queue_pop(). NOT a duration and
                             * deadline_s == 0 is NOT a "no deadline"
                             * sentinel: it means "already expired", since
                             * actor_now_s() is already > 0 shortly after
                             * boot. There is no "no TTL" value -- a caller
                             * that wants no practical deadline must pass
                             * one far enough in the future. */
} actor_cmd_t;

/* A small, capacity-ACTOR_QUEUE_MAX (4) command queue (spec section 8's
 * static budget). A compacting array, not a ring buffer with wraparound
 * indices -- at 4 slots that is simpler to get right and just as cheap.
 * actor_queue_pop() is where the two rules that matter most live:
 *
 *   - TTL: any entry whose deadline_s < now_s is dropped (never returned),
 *     counted in `expired`, and its identity latched into
 *     last_expired_dev/last_expired_action for the one caller
 *     (actor_service()) that can turn that into a named alert.
 *     deadline_s == now_s is still valid -- the deadline is inclusive.
 *   - Priority: among what is left, an ACTOR_SRC_SAFETY entry is returned
 *     before any older entry from another source -- a pending close must
 *     not queue behind an ordinary command ("a safety close jumps the
 *     queue"). Ties among multiple safety entries resolve FIFO, oldest
 *     first -- see test_two_safety_closes_resolve_fifo().
 *
 * actor_queue_push() has its own priority rule (review finding 1): when
 * the queue is full, an ACTOR_SRC_SAFETY push EVICTS the oldest
 * non-safety entry rather than being refused -- refusing it would strand
 * an actuator open, which is exactly what the safety close exists to
 * prevent. The evicted command's identity is latched into
 * last_evicted/last_evicted_valid, one-shot (cleared at the start of every
 * push), so actor_request() can alert on the displacement. A push is only
 * ever refused when every queued entry is ALSO a safety command -- a
 * genuine overload with nothing left to evict. */
typedef struct {
    actor_cmd_t cmds[ACTOR_QUEUE_MAX];
    uint8_t     count;
    uint32_t    expired;             /* cumulative TTL drops since init */
    int8_t      last_expired_dev;    /* identity of the most recent TTL   */
    uint8_t     last_expired_action; /* drop; -1/ACTION_NONE until one has
                                       * happened (see actor_queue_init()) */
    bool        last_evicted_valid;  /* true iff the most recent push
                                       * evicted an entry to make room */
    actor_cmd_t last_evicted;        /* the evicted command; valid iff
                                       * last_evicted_valid */
} actor_queue_t;

void     actor_queue_init(actor_queue_t *q);
/* False (queue unchanged) when the queue is full AND either cmd is not a
 * safety command, or every queued entry is itself a safety command (see
 * actor_queue_t's comment above for the eviction rule). Never inspects
 * deadline_s -- TTL is only evaluated on pop, so a full queue of
 * already-expired ordinary commands still refuses a new ordinary push
 * until the next actor_service() call drains it (see
 * test_full_queue_of_expired_commands_still_refuses_push()). */
bool     actor_queue_push(actor_queue_t *q, const actor_cmd_t *cmd);
bool     actor_queue_pop(actor_queue_t *q, actor_cmd_t *out, uint32_t now_s);
/* Cumulative count of TTL drops since actor_queue_init(), not "since the
 * last pop" -- a monitor polling this sees a running total. */
uint32_t actor_queue_expired(const actor_queue_t *q);

/* ---------------------------------------------------------------------
 * Pure decision logic (review fix round 1). actor_request()/
 * actor_service() below are thin ESP-IDF wrappers around these two
 * functions -- see this header's top comment.
 * --------------------------------------------------------------------- */

typedef struct {
    actor_verdict_t verdict;      /* ACTOR_OK, or the guard that refused --
                                    * see actor_table_check() */
    bool            queued;       /* true iff the command is now in *q */
    bool            evicted;      /* true iff queuing it evicted another
                                    * command (verdict == ACTOR_OK and
                                    * queued == true whenever this is true) */
    actor_cmd_t     evicted_cmd;  /* valid iff evicted */
} actor_request_result_t;

/* The decision actor_request() makes, minus the lock/clock/alert_post().
 * Checks the table, and on ACTOR_OK pushes onto the queue (which may
 * evict, see actor_queue_t's comment). Does NOT call actor_table_record()
 * -- recording happens at dispatch time, in actor_service_step(), per
 * review finding 2 below. */
actor_request_result_t actor_request_decide(actor_table_t *t, actor_queue_t *q,
    int dev_idx, uint8_t action_id, uint16_t param, actor_source_t source,
    uint32_t deadline_s, uint32_t now_s);

typedef struct {
    bool            dispatched;         /* true iff cmd should be sent */
    actor_cmd_t     cmd;                /* valid iff dispatched */
    uint32_t        ttl_dropped;        /* TTL drops during this call (0+) */
    int8_t          ttl_last_dev;       /* identity of the last one, if any */
    uint8_t         ttl_last_action;
    bool            redecline;          /* a command was popped but refused
                                          * on RE-check (see below) */
    actor_verdict_t redecline_verdict;
    int8_t          redecline_dev;
    uint8_t         redecline_action;
    uint16_t        redecline_param;
} actor_service_result_t;

/* The decision actor_service() makes, minus the lock/clock/alert_post()/
 * dispatch hook. Pops the next due command (actor_queue_pop()), then --
 * review finding 2 -- RE-RUNS actor_table_check() before recording or
 * dispatching it. The queue check at enqueue time and the dispatch here
 * can be arbitrarily far apart (another command may have been recorded
 * in between, or an operator may have set lockout), so a command that was
 * ACTOR_OK when queued is not assumed still valid; if the re-check
 * refuses it, it is dropped (never recorded, never dispatched) and
 * reported via redecline_* instead. Only a command that re-checks OK is
 * recorded (actor_table_record()) and marked dispatched -- recording at
 * dispatch time, not at enqueue time, is what makes the re-check
 * meaningful: the hourly budget is spent once, at the moment a command is
 * actually handed onward. */
actor_service_result_t actor_service_step(actor_table_t *t, actor_queue_t *q, uint32_t now_s);

/* Sets up the shared table and queue this file owns. Call once at boot,
 * before any actor_request()/actor_service() call. */
void actor_init(void);

/* actor_now_s(): the single clock actor_request()'s callers (Tasks 9, 10,
 * 11) MUST derive deadline_s from. It is uptime seconds (esp_timer-based),
 * the same clock actor_table_t's cooldown/rate windows already use -- NOT
 * wall-clock/epoch time. A caller that computes a deadline from
 * timekeeper_now() (epoch seconds) instead of this will get a deadline_s
 * that is either already in the past or absurdly far in the future
 * relative to actor_now_s(), and every such command will silently expire
 * on arrival (see actor_cmd_t.deadline_s's comment on why 0 is not a "no
 * deadline" escape hatch either). */
uint32_t actor_now_s(void);

/* Lock-taking wrappers around the shared actor_table_t this file owns
 * (review finding 4 -- there is no raw-pointer accessor to it). Boot
 * wiring calls actor_declare()/actor_configure_guards() to populate the
 * table; the HTTP API (Task 11) calls actor_set_lockout() for the
 * operator's stop button. All three take the same mutex
 * actor_request()/actor_service() use, so a table mutation can never race
 * a concurrent actor_table_check()/actor_table_record() and observe a
 * half-written 16 B slot. See actor_table.h for what each call means. */
bool     actor_declare(int dev_idx, uint8_t action_id, uint16_t param_max, uint8_t flags);
bool     actor_configure_guards(int dev_idx, uint8_t action_id,
                                 uint16_t cooldown_s, uint8_t max_per_hour);
void     actor_set_lockout(int dev_idx, bool on);
uint32_t actor_full_drops(void);

/* The single door onto an actuator (spec section 3). Calls
 * actor_request_decide() under lock; on refusal (guard OR a full queue),
 * posts a named alert and returns false. On success -- including when
 * queuing evicted another command -- returns true; an eviction also posts
 * its own alert. Never a silent failure. */
bool actor_request(int dev_idx, uint8_t action_id, uint16_t param,
                    actor_source_t source, uint32_t deadline_s);

/* Registered by whichever module actually owns the radio (the GATT
 * command engine, Task 8); actor_service() calls this once per dispatched
 * command. A NULL hook is a safe no-op -- same convention as alert.h's
 * alert_wake_fn_t and gatt_engine.h's scan-resume hook. */
typedef void (*actor_dispatch_fn_t)(const actor_cmd_t *cmd);
void actor_set_dispatch_hook(actor_dispatch_fn_t fn);

/* Services the queue: calls actor_service_step() under lock, then outside
 * the lock posts a named alert for any TTL drop and for a dispatch-time
 * re-check refusal, and finally hands a dispatched command to the
 * dispatch hook. Call periodically from a task that can own the radio. */
void actor_service(void);
