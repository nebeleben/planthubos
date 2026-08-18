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
 * (the FreeRTOS-touching front door). actor_request()/actor_service()
 * live in this same file, but every ESP-IDF-only line inside actor.c (the
 * mutex, esp_timer_get_time(), the alert_post() call, the eventual GATT
 * hand-off) is `#ifdef ESP_PLATFORM`-gated exactly the way
 * unknown_capture.c already gates its own mutex -- so this whole file,
 * not just the queue struct, compiles and links with plain `cc` for the
 * host test, with no FreeRTOS/ESP-IDF toolchain required. */

#define ACTOR_QUEUE_MAX 4

typedef struct {
    int8_t   dev_idx;
    uint8_t  action_id;
    uint8_t  source;       /* actor_source_t, see actor_table.h */
    uint16_t param;
    uint32_t deadline_s;   /* absolute uptime seconds; see actor_queue_pop() */
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
 *     first -- see test_two_safety_closes_resolve_fifo(). */
typedef struct {
    actor_cmd_t cmds[ACTOR_QUEUE_MAX];
    uint8_t     count;
    uint32_t    expired;             /* cumulative TTL drops since init */
    int8_t      last_expired_dev;    /* identity of the most recent TTL   */
    uint8_t     last_expired_action; /* drop; -1/ACTION_NONE until one has
                                       * happened (see actor_queue_init()) */
} actor_queue_t;

void     actor_queue_init(actor_queue_t *q);
/* False (queue unchanged) when the queue already holds ACTOR_QUEUE_MAX
 * commands. Never inspects deadline_s -- TTL is only evaluated on pop, so
 * a full queue of already-expired commands still refuses a new push until
 * the next actor_service() call drains it (see
 * test_full_queue_of_expired_commands_still_refuses_push()). */
bool     actor_queue_push(actor_queue_t *q, const actor_cmd_t *cmd);
bool     actor_queue_pop(actor_queue_t *q, actor_cmd_t *out, uint32_t now_s);
/* Cumulative count of TTL drops since actor_queue_init(), not "since the
 * last pop" -- a monitor polling this sees a running total. */
uint32_t actor_queue_expired(const actor_queue_t *q);

/* Sets up the shared table and queue this file owns. Call once at boot,
 * before any actor_request()/actor_service() call. */
void actor_init(void);

/* The shared actor_table_t instance actor_request() checks against.
 * Exposed so boot wiring can populate it (actor_table_add()) and the HTTP
 * API (Task 11) can adjust guards (actor_table_set_guards()/
 * actor_table_set_lockout()). actor_table.c itself does no locking, by
 * design (see its own header) -- this module only serializes ITS OWN
 * access to the table (inside actor_request()/actor_service()); a caller
 * that mutates the table concurrently with those is responsible for its
 * own synchronization, same as any other actor_table_t owner. */
actor_table_t *actor_table_get(void);

/* The single door onto an actuator (spec section 3). Calls
 * actor_table_check(); on refusal, posts an alert naming the guard that
 * refused and returns false without touching the queue. On ACTOR_OK,
 * pushes the command and returns true, or -- if the queue is full --
 * posts an alert and returns false. Never a silent failure either way. */
bool actor_request(int dev_idx, uint8_t action_id, uint16_t param,
                    actor_source_t source, uint32_t deadline_s);

/* Registered by whichever module actually owns the radio (the GATT
 * command engine, Task 8); actor_service() calls this once per popped,
 * still-valid command. A NULL hook is a safe no-op -- same convention as
 * alert.h's alert_wake_fn_t and gatt_engine.h's scan-resume hook. */
typedef void (*actor_dispatch_fn_t)(const actor_cmd_t *cmd);
void actor_set_dispatch_hook(actor_dispatch_fn_t fn);

/* Services the queue: pops the next due command (see actor_queue_pop()'s
 * TTL and priority rules above), posting a named alert for any TTL drop
 * encountered along the way, then -- if a command survived -- records the
 * fire (actor_table_record()) and hands it to the dispatch hook. Recording
 * happens once per dispatch ATTEMPT, not once per confirmation, so the
 * hourly budget is spent on send attempts rather than only on confirmed
 * successes. Call periodically from a task that can own the radio. */
void actor_service(void);
