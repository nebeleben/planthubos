#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "actor_table.h"
#include "capability.h" /* device_kind_t -- actor_set_dispatch_hook() below is now
                          * keyed on it (M6b Task 7). Pulls in nothing beyond plain
                          * typedefs/macros (no ESP-IDF), so this stays link-clean for
                          * the host tests that include this header without ESP-IDF. */

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
    /* Set only by actor_request_retry(): this command was already
     * dispatched once and never reached the radio (M5b Task 8 -- the other
     * owner of the hub's single outbound connection had it), so it may be
     * put back on this queue exactly once more.
     *
     * The bit lives ON THE COMMAND rather than in a side table at the
     * dispatcher, and fix round 2 is why: a single-slot "which command did
     * I already retry" latch is defeated by two commands in flight (one
     * rule opening two valves, or a safety close alongside a rule
     * command). A requeues, the latch holds A; B is popped, does not
     * match, requeues, and destroys A's latch; A is popped and is no
     * longer recognised as a retry, so it requeues again -- ping-ponging at
     * decoder-tick rate and re-charging actor_table_record() every pass,
     * which is exactly the budget burn the latch existed to prevent.
     * Travelling with the command makes that unreachable by construction,
     * for any number of commands in flight, and needs no identity matching.
     * Free in memory: it fills padding actor_cmd_t already had. */
    uint8_t  retried;
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
/* The retry bit above cost nothing: it fills the hole that already sat
 * between `source` and the 2-byte-aligned `param`. Pinned, because
 * ACTOR_QUEUE_MAX of these is a line in spec section 8's static budget and
 * a future field added in the wrong place would silently cost 4 B x 4. */
_Static_assert(sizeof(actor_cmd_t) == 12, "spec section 8 budgets 12 B per queued command");

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
 * review finding 2 below.
 *
 * `retried` is stamped onto the queued command (actor_cmd_t.retried) and
 * is false for every ordinary request; only actor_request_retry() passes
 * true. It changes NOTHING about the decision -- a retry faces exactly the
 * same guards, in the same order, as any other command -- it only travels
 * with the command so the dispatcher can tell a first attempt from a
 * second one. */
actor_request_result_t actor_request_decide(actor_table_t *t, actor_queue_t *q,
    int dev_idx, uint8_t action_id, uint16_t param, actor_source_t source,
    uint32_t deadline_s, uint32_t now_s, bool retried);

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
/* Undeclares dev_idx entirely -- see actor_table_remove() for the full
 * contract and for why this is deliberately blunt (the device's guards,
 * spent budget and lockout all go with it) and must only be called on
 * evidence that the device is no longer an actuator at all. M5b Task 8's
 * wrapper reindex is the one caller. */
bool     actor_undeclare(int dev_idx);
uint32_t actor_full_drops(void);
/* Lock-taking wrapper around actor_table_action_flags() -- M5b Task 9's one
 * reader of a declared pair's flags (ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF, see
 * actor_table.h), used to decide whether a dispatched timed-open action
 * needs a hub-scheduled close at all. */
bool     actor_action_flags(int dev_idx, uint8_t action_id, uint8_t *flags_out);

/* Lock-taking wrappers around actor_table_pair_state()/actor_table_lockout()
 * (actor_table.h -- M5b Task 11) -- the READ side this list was missing
 * (see this comment's own top note: three mutating wrappers, no read
 * accessor, because nothing needed one before the HTTP API did). The
 * httpd task (GET /api/v1/devices' actions[], Task 11) is the intended
 * caller; both take the identical mutex actor_request()/actor_service()
 * use, so a read here can never race a concurrent actor_table_check()/
 * actor_table_record() and observe a half-written slot. See
 * actor_table_pair_state()'s doc comment for exactly what each field of
 * actor_pair_state_t means. */
bool     actor_pair_state(int dev_idx, uint8_t action_id, actor_pair_state_t *out);
bool     actor_lockout(int dev_idx, bool *out);

/* ---------------------------------------------------------------------
 * Whole-branch review, ruling FINAL-persist: the lock-taking half of guard
 * persistence. actor_persist.c owns the file and calls these; the table
 * itself is reached only through them, the same discipline every other
 * accessor in this header is held to.
 * --------------------------------------------------------------------- */

/* Records `dev_idx`'s stable identity (actor_table.h's ACTOR_DEVICE_KEY_LEN
 * device_id_t bytes) so its guards can be persisted and found again after a
 * reboot. Called by whoever declares the device as an actuator. */
void     actor_set_device_key(int dev_idx, const uint8_t key[ACTOR_DEVICE_KEY_LEN]);

/* actor_table_device_key() / actor_table_find_by_key() under the lock.
 * actor_find_by_key() returning -1 means "no DECLARED device carries this
 * identity right now", which is exactly the condition pending_close treats
 * as DEFERRED -- not an error, and never a licence to act on some other
 * index. */
bool     actor_device_key(int dev_idx, uint8_t out[ACTOR_DEVICE_KEY_LEN]);
int      actor_find_by_key(const uint8_t key[ACTOR_DEVICE_KEY_LEN]);

/* Test-and-clear: has anything the guard file records changed since the
 * last call? Set by actor_configure_guards(), actor_set_lockout(),
 * actor_undeclare() and by every dispatch that charges an activation. */
bool     actor_guards_take_dirty(void);

/* The same question WITHOUT clearing -- for a caller that needs to bring
 * the persisted image up to date before reading it, but must leave the
 * obligation to actually write the file where it already is
 * (actor_persist_service(), on the decoder loop). See
 * actor_persist_sync(). */
bool     actor_guards_dirty(void);

/* actor_table_guard_merge() / actor_table_guard_apply() under the lock.
 * See their contracts in actor_table.h -- including, for apply(), what the
 * restored uptime timestamps are taken to mean. */
size_t   actor_guards_merge(actor_guard_row_t *rows, size_t n, size_t cap);
bool     actor_guards_apply(int dev_idx, const actor_guard_row_t *row);

/* The single door onto an actuator (spec section 3). Calls
 * actor_request_decide() under lock; on refusal (guard OR a full queue),
 * posts a named alert and returns false. On success -- including when
 * queuing evicted another command -- returns true; an eviction also posts
 * its own alert. Never a silent failure. */
bool actor_request(int dev_idx, uint8_t action_id, uint16_t param,
                    actor_source_t source, uint32_t deadline_s);

/* The same door, for a command that was already dispatched once and never
 * reached the actuator because the radio belonged to something else at
 * that instant (M5b Task 8; ble_collector.c is the only caller). NOT a
 * second way onto an actuator: it runs the identical actor_request_decide()
 * with the identical guards -- lockout, cooldown, rate and bound all still
 * apply -- and differs only in stamping actor_cmd_t.retried, so this
 * command cannot be put back a third time.
 *
 * Pass the ORIGINAL source and deadline_s. The deadline is what keeps a
 * retry honest: it is absolute, so a command that has been bouncing around
 * long enough to become a hazard expires in the queue instead of being
 * executed late (spec section 4.1).
 *
 * Callers other than a dispatcher that has just been told "the radio was
 * busy" want actor_request(); a device that refused the write is not a
 * retryable condition and must not be laundered into one. */
bool actor_request_retry(int dev_idx, uint8_t action_id, uint16_t param,
                          actor_source_t source, uint32_t deadline_s);

/* Registered by whichever module actually owns the radio for a given
 * device kind (the GATT command engine for DEV_KIND_BLE, M5b Task 8; a
 * Zigbee dispatcher for DEV_KIND_ZIGBEE is M6b's reason for this widening).
 * actor_service() resolves the dispatched command's device kind and calls
 * THAT kind's hook, once per dispatched command -- the single s_dispatch
 * pointer M5b had, when GATT was the only radio that could carry an
 * action, is now a small per-kind table indexed by device_kind_t. An
 * unregistered kind's hook is NULL, the same safe no-op posture a NULL
 * hook already had through M5b -- same convention as alert.h's
 * alert_wake_fn_t and gatt_engine.h's scan-resume hook -- except
 * actor_service() also posts ALERT_CODE_NO_DISPATCHER when that happens,
 * so a command is never silently dropped just because no dispatcher has
 * registered for its kind yet (see actor_service()'s doc comment below).
 * This is still the only exit a dispatched command has, not a second door
 * onto the radio -- see this header's top comment. */
typedef void (*actor_dispatch_fn_t)(const actor_cmd_t *cmd);
void actor_set_dispatch_hook(device_kind_t kind, actor_dispatch_fn_t fn);

/* Services the queue: calls actor_service_step() under lock, then outside
 * the lock posts a named alert for any TTL drop and for a dispatch-time
 * re-check refusal, and finally resolves the dispatched command's device
 * kind and hands it to that kind's dispatch hook (M6b Task 7). A kind
 * whose hook was never registered (or whose device's kind cannot be
 * resolved at all) posts ALERT_CODE_NO_DISPATCHER and drops the command
 * instead of calling nothing -- never a silent disappearance. Call
 * periodically from a task that can own the radio. */
void actor_service(void);

/* ---------------------------------------------------------------------
 * Pending close: persistence and boot replay (M5b Task 9, spec section 4.5).
 *
 * Some actuators close themselves a fixed duration after being opened --
 * ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF (actor_table.h), the DIY profile's
 * preferred and mandatory path (spec section 4.3). For everyone else, the
 * HUB owns the close: it must send ACT_SWITCH_OFF itself once the
 * parameter (a duration, action.h's ACTION_PARAM_DURATION_S) elapses, and
 * that obligation must survive a reboot between the open and the close --
 * a hub that opened a valve, crashed, and rebooted with no memory of it
 * would leave that valve open indefinitely. This is the module that closes
 * that gap: pending_close_arm() records the obligation (RAM + one small
 * flash write); a pump polled from ble_collector.c's decoder loop
 * (pending_close_service(), the impure half, in pending_close.c) attempts
 * the close, retries with bounded backoff, and alerts CRITICAL
 * (ALERT_CODE_CLOSE_UNCONFIRMED) on exhaustion; pending_close_clear()
 * removes the obligation once the close is CONFIRMED (not merely
 * dispatched -- spec section 4.4 treats a write that landed but was not
 * confirmed as still open) and rewrites/deletes the file.
 *
 * Split, like the rest of this component: everything below down to
 * pending_close_deserialize() is pure C99 (a small RAM table, no ESP-IDF),
 * directly host-tested by tests/host/test_pending_close.c -- including,
 * per fix round 1, the retry-counting and exhaustion POLICY itself
 * (pending_close_step(), below), not just the table it operates on. Only
 * pending_close_init()/pending_close_service() (LittleFS, actor_request(),
 * alert_post()) are impure, gated `#ifdef ESP_PLATFORM` exactly like
 * actor_request()/actor_service() above -- see pending_close.c. Both of
 * those, plus pending_close_note_result(), are called ONLY from
 * ble_collector.c's adv_decoder_task -- fix round 1 moved the GATT
 * completion hook's calls (which run on the NimBLE host task, where
 * flash must never be touched) to a deferred flag drained on that task,
 * the same pattern ble_collector.c already uses for s_requeue/
 * s_cmd_state_pending. That single-task discipline is also what makes
 * this module's RAM table safe with no lock of its own: unlike
 * actor_table_t (genuinely cross-task, and locked in actor.c), every
 * mutation of pending_close.c's table now happens on one task.
 *
 * Deliberately NOT written back to flash on every retry (spec section 8's
 * flash-wear budget): a fresh arm() is the only write while a close is
 * outstanding; retries/backoff live in RAM only, so a reboot mid-retry
 * simply restarts the retry count -- harmless, since boot replay treats
 * every surviving record as due immediately regardless. */

#define PENDING_CLOSE_MAX         ACTOR_MAX_DEVICES /* one pending close per
                                                       * device, and at most
                                                       * ACTOR_MAX_DEVICES
                                                       * devices are ever
                                                       * actuators */
#define PENDING_CLOSE_MAX_RETRIES 5                  /* bounded attempts
                                                       * against a KNOWN
                                                       * device before the
                                                       * obligation is given
                                                       * up (for this boot)
                                                       * as CRITICAL -- see
                                                       * pending_close_step() */
#define PENDING_CLOSE_UNKNOWN_RECHECK_S 5u            /* re-check interval
                                                       * while the device is
                                                       * not yet declared;
                                                       * does NOT count
                                                       * against the retry
                                                       * budget above */
#define PENDING_CLOSE_DEFERRED_ALERT_S  120u          /* fix round 2: how long
                                                       * a record may stay
                                                       * continuously DEFERRED
                                                       * (never once reaching
                                                       * a known device) before
                                                       * it is loud about it --
                                                       * see pending_close_step()'s
                                                       * PENDING_CLOSE_STEP_DEFERRED_TIMEOUT.
                                                       * 120 s = 24 recheck
                                                       * cycles at
                                                       * PENDING_CLOSE_UNKNOWN_RECHECK_S:
                                                       * comfortably longer than
                                                       * any plausible
                                                       * rediscovery (BLE
                                                       * advertisement/scan
                                                       * intervals in this
                                                       * codebase are
                                                       * sub-second to a few
                                                       * seconds; even the
                                                       * boot-time "device not
                                                       * scanned yet" window is
                                                       * bounded by
                                                       * CONFIG_PLANTHUB_BLE_SCAN_ITVL/WINDOW,
                                                       * milliseconds), short
                                                       * enough that an
                                                       * operator who left a
                                                       * valve open next to an
                                                       * unreachable device
                                                       * finds out within two
                                                       * minutes rather than
                                                       * discovering it only
                                                       * at PENDING_CLOSE_MAX_RETRIES'
                                                       * exhaustion -- which,
                                                       * per this ruling, never
                                                       * even happens for a
                                                       * device that stays
                                                       * unreachable. */

/* THE KEY IS THE DEVICE, NOT ITS REGISTRY INDEX (whole-branch review
 * follow-up). This record used to carry an `int8_t dev_idx` and persist it.
 * The registry is RAM-only (data_core.c's `static registry_t s_registry`)
 * and claims slots in DISCOVERY ORDER, so index N after a reboot is
 * whichever device happened to advertise Nth -- not the one that was open.
 * The obligation then replayed against the wrong row and failed one of two
 * ways: the device at that index has no switch.off, so the record deferred
 * forever with the valve open and one misleading "unreachable" alert; or
 * that index held a DIFFERENT actuator that does have switch.off, and the
 * hub closed the wrong actuator while the real valve stayed open. The
 * second is a physical action against a device nobody commanded, taken
 * while the hazard it was meant to end continues.
 *
 * So the identity persisted is the device's own (device_id_t bytes,
 * ACTOR_DEVICE_KEY_LEN -- the same key actor_persist.h uses, deliberately
 * one mechanism and not two), and it is resolved back to a live index at
 * the moment it is needed, via actor_find_by_key(). An identity that
 * resolves to nothing is DEFERRED, which is the state this module already
 * handles correctly, right down to the once-per-streak
 * PENDING_CLOSE_STEP_DEFERRED_TIMEOUT alert -- so a device that is gone for
 * good is still loud rather than silent.
 *
 * All-zero is the free-row / not-a-record sentinel, exactly as in
 * actor_table.h: it is not a device_id_t this hub can address. */
#define PENDING_CLOSE_KEY_LEN ACTOR_DEVICE_KEY_LEN

typedef struct {
    uint32_t deadline_s;   /* actor_now_s()-scale: when this obligation is
                             * next due to be attempted (NOT re-persisted on
                             * every retry -- see this section's top comment) */
    uint8_t  key[PENDING_CLOSE_KEY_LEN];
    uint8_t  close_action;
    uint8_t  retries;
} pending_close_t;
/* 16 B, up from the 12 B spec section 8 budgeted when this record carried a
 * one-byte index instead of a nine-byte identity: 64 B for the whole table
 * rather than 48. Pinned, because it is a budgeted structure and because a
 * silent growth here is a silent growth in the file it serialises to. */
_Static_assert(sizeof(pending_close_t) == 16,
               "pending_close_t is 16 B once keyed on device identity (spec section 8)");

/* Arms (or re-arms) the obligation to close the device identified by `key`
 * via `close_action` at `deadline_s` (actor_now_s()-scale). Re-arming an
 * already-pending device REPLACES its record (one obligation per device: a
 * device that opens again while its previous close is still pending gets
 * the new deadline, not a second row) and resets its retry count to 0 -- a
 * fresh open is a fresh obligation, not a continuation of a stale retry
 * sequence. Writes the persisted file (atomically -- see pending_close.c).
 * A NULL or all-zero key is a no-op (nothing identifiable to arm). Callable
 * ONLY from adv_decoder_task on target (see this section's top comment);
 * host tests call it directly. */
void   pending_close_arm(const uint8_t key[PENDING_CLOSE_KEY_LEN],
                          uint8_t close_action, uint32_t deadline_s);

/* Removes that device's obligation, if any (a safe no-op if none is
 * pending), and rewrites the persisted file -- deleting it entirely once
 * the table is empty, so an unopened hub leaves no stray file behind. The
 * one caller that matters is a CONFIRMED close (pending_close_note_result(),
 * below); calling it for any other reason abandons the obligation without
 * having actually closed anything. Same single-task restriction as
 * pending_close_arm(). */
void   pending_close_clear(const uint8_t key[PENDING_CLOSE_KEY_LEN]);

/* Finds the earliest-deadline record whose deadline_s <= now_s (ties broken
 * by table order) and copies it into *out, WITHOUT removing it -- the
 * caller (pending_close_service()) advances or clears it explicitly via
 * pending_close_step()/pending_close_clear() once it knows the outcome of
 * attempting it. Returns 1 if a due record was found, 0 otherwise. `out`
 * may be NULL to just test whether anything is due. */
int    pending_close_due(uint32_t now_s, pending_close_t *out);

/* The pure retry-counting and exhaustion POLICY (fix round 1, finding 6):
 * given that the record for `key` was JUST found due by
 * pending_close_due(), and whether the caller could resolve that identity
 * to a currently-declared actuator (`device_known` -- actor_find_by_key()
 * followed by actor_action_flags()), decides what happens next and mutates
 * the record accordingly. Never touches the file.
 *
 *   PENDING_CLOSE_STEP_NONE      -- `key` has no pending record (should
 *                                    not happen right after due() found
 *                                    it, but defensive). *out untouched.
 *
 *   PENDING_CLOSE_STEP_DEFERRED  -- due, but device_known is false: the
 *                                    hub itself is not ready yet (the
 *                                    device has not been rediscovered),
 *                                    not the device refusing -- the same
 *                                    principle as M5b Task 8's radio-busy
 *                                    gate. Rescheduled
 *                                    PENDING_CLOSE_UNKNOWN_RECHECK_S
 *                                    ahead; `retries` is UNCHANGED --
 *                                    this does not spend the budget below.
 *                                    The unreachable-streak counter behind
 *                                    this (see fix round 2 below) advances.
 *
 *   PENDING_CLOSE_STEP_DEFERRED_TIMEOUT -- fix round 2: identical to
 *                                    PENDING_CLOSE_STEP_DEFERRED (same
 *                                    reschedule, `retries` still unchanged)
 *                                    EXCEPT this is the FIRST due-check at
 *                                    or past PENDING_CLOSE_DEFERRED_ALERT_S
 *                                    of continuous, unbroken DEFERRED
 *                                    outcomes for this record since it was
 *                                    (re)armed or boot-loaded. Fix round 1's
 *                                    own ruling (never spend the retry
 *                                    budget on "hub not ready") combined
 *                                    with the exhaustion ruling (never
 *                                    alert until retries are spent)
 *                                    produced exactly the silence both
 *                                    rulings existed to prevent: a device
 *                                    that never comes back stayed DEFERRED
 *                                    forever and NEVER alerted. This breaks
 *                                    that silence without touching either
 *                                    ruling: the retry budget is still
 *                                    untouched, the obligation still never
 *                                    exhausts or clears on its own -- the
 *                                    CALLER must alert EVENT_LEVEL_ALERT
 *                                    (not CRITICAL: "never been able to try"
 *                                    is a weaker claim than "tried and the
 *                                    device refused") for *out, exactly
 *                                    ONCE per continuous unreachable streak
 *                                    -- this function returns ordinary
 *                                    DEFERRED for every check after this one
 *                                    until the streak is broken (a REQUEST,
 *                                    or a fresh arm) and, if it never is,
 *                                    resumes again. Never touches the file
 *                                    or the retry budget; this function
 *                                    never calls alert_post() itself.
 *
 *   PENDING_CLOSE_STEP_REQUEST   -- device_known is true and retries are
 *                                    still under PENDING_CLOSE_MAX_RETRIES:
 *                                    `retries` is incremented and the
 *                                    deadline advanced by bounded
 *                                    exponential backoff. The CALLER must
 *                                    now call actor_request() for *out --
 *                                    this function never does (it has no
 *                                    ESP-IDF dependency). The unreachable
 *                                    streak resets to 0 -- the device was
 *                                    reached, so the silence this guards
 *                                    against is over (a real refusal from
 *                                    here on is EXHAUSTED's job, below).
 *
 *   PENDING_CLOSE_STEP_EXHAUSTED -- retries were ALREADY at
 *                                    PENDING_CLOSE_MAX_RETRIES on entry, so
 *                                    NO new attempt is issued this call --
 *                                    this can only happen on a call that
 *                                    follows the FINAL attempt's own
 *                                    backoff period (due() gates every
 *                                    call), so by construction the alert
 *                                    below never fires in the same pass as
 *                                    the attempt whose outcome it is
 *                                    reporting on (fix round 1, finding 5).
 *                                    The record is marked to never be due
 *                                    again THIS BOOT, but is left otherwise
 *                                    untouched -- NOT cleared, NOT rewritten
 *                                    to the file (fix round 1, finding 2):
 *                                    the obligation the file already
 *                                    recorded survives, so the NEXT boot
 *                                    tries again from scratch. The CALLER
 *                                    must alert EVENT_LEVEL_CRITICAL for
 *                                    *out; this function never calls
 *                                    alert_post() itself.
 *
 * Fix round 1's own note that a device never rediscovered "stays DEFERRED
 * forever ... and never alerts" is now stale: it still stays DEFERRED
 * forever (the retry budget is still never spent on it, and it still never
 * exhausts or clears), but PENDING_CLOSE_STEP_DEFERRED_TIMEOUT fires once,
 * comfortably before an operator would otherwise have no way to know. */
typedef enum {
    PENDING_CLOSE_STEP_NONE = 0,
    PENDING_CLOSE_STEP_DEFERRED,
    PENDING_CLOSE_STEP_DEFERRED_TIMEOUT,
    PENDING_CLOSE_STEP_REQUEST,
    PENDING_CLOSE_STEP_EXHAUSTED,
} pending_close_step_t;
pending_close_step_t pending_close_step(const uint8_t key[PENDING_CLOSE_KEY_LEN],
                                         uint32_t now_s, bool device_known,
                                         pending_close_t *out);

/* On boot, every surviving record is due immediately, whatever deadline it
 * recorded -- the hub cannot know how long it was off. True for any record
 * carrying a key (a free/invalid slot, all-zero key, is never boot-due). */
bool   pending_close_is_boot_due(const pending_close_t *r);

/* Populates the RAM table from `recs` (as read from the persisted file,
 * already deserialized), treating every record for which
 * pending_close_is_boot_due() is true as due AT ONCE (its stored deadline_s
 * is discarded, not consulted) -- the pure half of boot replay. Does not
 * touch the file. The impure half (pending_close_init(), in pending_close.c)
 * reads the file, calls this, then attempts every now-due record. */
void   pending_close_boot_load(const pending_close_t *recs, size_t n);

/* Number of devices with an obligation currently pending
 * (0..PENDING_CLOSE_MAX), INCLUDING one that has reached
 * PENDING_CLOSE_STEP_EXHAUSTED for this boot -- it is still an obligation,
 * just not one being retried until the next boot. */
size_t pending_close_active_count(void);

/* Fix round 3, finding 1: the exact array pending_close_save() persists --
 * every currently-pending record, with `retries` forced to 0 (capped at
 * `cap`; returns the count written). `retries` is session-only RAM state
 * (this section's own top comment), so what reaches disk must always be a
 * FULL, untouched retry budget, regardless of how far the live row has
 * actually retried or whether it has already reached
 * PENDING_CLOSE_STEP_EXHAUSTED -- persisting the live counter would flush
 * an exhausted row to disk (triggered by any OTHER device's arm/clear) and
 * the next boot's first due-check would then read it as already-exhausted
 * and post CRITICAL having made zero attempts that boot, exactly the
 * "pre-exhausted on arrival" outcome the round-1 ruling (leave the
 * obligation for the next boot to try again) existed to prevent. Pure and
 * host-tested directly -- not merely inferred from pending_close_save()'s
 * body, which cannot itself be host-tested (LittleFS). */
size_t pending_close_persist_snapshot(pending_close_t *out, size_t cap);

/* Pure predicate (fix round 1, finding 6): does an open of `action_id` need
 * the hub to schedule its own close? True iff the action takes a duration
 * (action.h's ACTION_PARAM_DURATION_S) AND `flags` -- whatever
 * actor_action_flags() reported for the declared pair -- does NOT carry
 * ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF (actor_table.h: the device closes
 * itself). False for an unknown action_id. A caller that could not resolve
 * the pair at all (actor_action_flags() itself returned false) must not
 * call this. */
bool   pending_close_needed(uint8_t action_id, uint8_t flags);

/* Pure predicate, whole-branch review (Critical 1 + Important 2, ruling
 * FINAL-arm): should the obligation be armed for THIS command, at the
 * moment it is handed to the radio?
 *
 * The obligation used to be armed from the GATT completion hook, and only
 * on the branch where the whole attempt reached GS_DONE. But the state
 * machine WRITES FIRST and CONFIRMS SECOND, so the write is already on the
 * device when the confirm's `require` is unsatisfied (GF_CONFIRM_FAILED),
 * when the confirm read is shorter than the compared field (GF_SHORT_READ),
 * and when an error/timeout/disconnect arrives in GS_READING. Every one of
 * those reports ok == false, so the valve was open with NO obligation
 * recorded anywhere -- while command_finish() still submitted the confirm
 * read's value, so switch.state read 0 and the dashboard, MQTT, Home
 * Assistant and InfluxDB all reported it closed. Even on the success path
 * the arm was deferred to a later decoder tick, so a brownout in that
 * window lost it -- the exact gap spec section 4.5 exists to close.
 *
 * So the obligation is now armed BEFORE dispatch instead. Any post-write
 * failure therefore already has one, and the crash window shrinks to
 * before the valve physically moves. The cost is a spurious obligation
 * when a command never reaches the radio at all, which discharges by
 * closing an already-closed valve -- harmless in the only direction that
 * matters.
 *
 * True iff `source` is not ACTOR_SRC_SAFETY (a close does not arm its own
 * close) and pending_close_needed() holds for the pair. */
bool   pending_close_arm_on_dispatch(actor_source_t source, uint8_t action_id,
                                     uint8_t flags);

/* On-disk format: `{ u8 fmt=2; u8 count; u16 crc }` followed by `count`
 * fixed 15-byte records (key[9], close_action, deadline_s LE32, retries).
 *
 * FORMAT 2, and format 1 is DISCARDED rather than reinterpreted. Format 1's
 * record began with an `int8_t dev_idx`; reading those bytes as the head of
 * a device key is exactly the class of mistake this re-keying exists to
 * prevent, and the cost of discarding is at most one stale obligation on
 * the single upgrade boot -- against a wrong close, which is a physical
 * action on a device nobody commanded. pending_close_init() deletes the
 * unreadable file after logging it, so the warning does not repeat forever.
 * A record whose key is the all-zero sentinel is refused too: nothing could
 * ever resolve it.
 * pending_close_serialize() returns the number of BYTES written into `buf`
 * (0 on any failure: `n` too large to fit `cap`, or more than 255 records --
 * count is one wire byte -- writes NOTHING rather than a truncated file).
 * pending_close_deserialize() returns the number of RECORDS recovered into
 * `out` (capped at `cap`), or 0 for ANY of: a short/absent header, an
 * unrecognised fmt, a length that doesn't exactly match `4 + count*7` (a
 * truncated OR a trailing-garbage file), a crc mismatch, or `count > cap` --
 * a corrupt or truncated file yields NOTHING, never a plausible-looking
 * partial list, because closing a device the hub was never told about is
 * its own bug. */
size_t pending_close_serialize(const pending_close_t *recs, size_t n, uint8_t *buf, size_t cap);
size_t pending_close_deserialize(const uint8_t *buf, size_t len, pending_close_t *out, size_t cap);

/* Impure half (pending_close.c, `#ifdef ESP_PLATFORM`-gated internals).
 * BOTH are called only from ble_collector.c's adv_decoder_task -- see this
 * section's top comment.
 *
 * pending_close_init() -- call once at boot, after actor_init(), still on
 * the task that calls ble_collector_start() (before adv_decoder_task
 * exists, so still single-threaded with respect to it). Always resets the
 * RAM table first (unconditionally, so it is also the test suite's reset
 * hook -- see test_pending_close.c). On target, then reads the persisted
 * file, calls pending_close_boot_load(), and immediately attempts every
 * now-due record (pending_close_service()).
 *
 * pending_close_service() -- call periodically (ble_collector.c's decoder
 * loop, unconditionally on role: it costs nothing but a table scan when
 * nothing is due, and a node with no GATT radio must still be able to
 * retry/back off/exhaust a stale obligation rather than leave it silently
 * stuck after one boot-time attempt). For each due record: resolves
 * device_known via actor_action_flags(), calls pending_close_step(), and
 * on PENDING_CLOSE_STEP_REQUEST calls actor_request(..., ACTOR_SRC_SAFETY,
 * ...) -- the same door, exempt from every rate-shaping guard
 * (actor_table.h) -- or on PENDING_CLOSE_STEP_EXHAUSTED posts
 * EVENT_LEVEL_CRITICAL/ALERT_CODE_CLOSE_UNCONFIRMED, or (fix round 2) on
 * PENDING_CLOSE_STEP_DEFERRED_TIMEOUT posts
 * EVENT_LEVEL_ALERT/ALERT_CODE_CLOSE_DEVICE_UNREACHABLE. Never clears on
 * exhaustion, and PENDING_CLOSE_STEP_DEFERRED_TIMEOUT never touches the
 * record beyond what ordinary DEFERRED already does (see
 * pending_close_step()'s doc comment). */
void   pending_close_init(void);
void   pending_close_service(void);

/* Called ONLY from ble_collector.c's adv_decoder_task, once per dispatched
 * command whose source was ACTOR_SRC_SAFETY, with that command's real
 * outcome -- see gatt_engine.h's gatt_cmd_done_fn_t doc comment for what
 * ok/confirmed mean. (The GATT completion hook itself runs on the NimBLE
 * host task and must not call this directly -- fix round 1, finding 1 --
 * it latches the outcome into a deferred flag that adv_decoder_task drains
 * before calling this, the same pattern ble_collector.c already uses for
 * s_requeue/s_cmd_state_pending.) Only ok=true AND confirmed=true clears
 * the obligation: any other outcome (ok=false, or ok=true/confirmed=false
 * -- "completed unconfirmed", spec section 4.4) is deliberately left
 * alone here -- the obligation stays pending and pending_close_service()'s
 * own retry/backoff picks it up on the next pass, with no separate
 * failure path to keep in sync. */
void   pending_close_note_result(const uint8_t key[PENDING_CLOSE_KEY_LEN],
                                  bool ok, bool confirmed);
