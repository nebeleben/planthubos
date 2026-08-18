#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "action.h"

/* The safety core's report path (M5b Task 5). Actuator alerts -- a close
 * that could not be confirmed, a guard refusal, a command dropped for
 * expiry -- need somewhere to go that:
 *
 *   - producers on several tasks (the NimBLE host task, an esp_timer
 *     callback, the httpd task) can reach WITHOUT blocking (an esp_timer
 *     callback must not block, so this is a critical section, not a
 *     mutex), and
 *   - never touches flash from a task whose stack can't afford it. M5a had
 *     to cut its event-log write entirely because the BLE decoder task's
 *     3072 B stack could not hold event_log_append()'s LittleFS fopen/
 *     fwrite chain plus the SSE/MQTT hooks (~1.5-2 KB). The rules engine
 *     task's stack (8192 B, measured 1636 B free) is the one place sized
 *     for that chain, because it already pays it for its own
 *     event_log_append() call inside psvm_run.
 *
 * So the ring is split in two: alert_post() (this header) is a fixed-size
 * record append under a critical section -- cheap and shallow, safe from
 * any task, nothing that touches the radio may touch flash. alert_drain()
 * is called by the rules engine task alone, at the top of each evaluation
 * pass, and is the only thing that turns a queued alert into an
 * event_log_append() call. */

/* Not exhaustive on purpose -- Tasks 6, 7 and 9 add alert producers (the
 * actor table's guard refusals, the command queue's TTL drop, a safety
 * close that could not be confirmed) and may add codes as they need them;
 * this enum exists so callers don't have to invent ad hoc integers for the
 * codes already known about. `code` travels as a plain uint8_t on the
 * wire (alert_rec_t, alert_post()'s signature) so a later addition here
 * never changes either. */
typedef enum {
    ALERT_CODE_UNKNOWN = 0,        /* refused: unknown device/action */
    ALERT_CODE_BOUND,              /* refused: parameter over bound */
    ALERT_CODE_LOCKOUT,            /* refused: device locked out */
    ALERT_CODE_COOLDOWN,           /* refused: cooldown not elapsed */
    ALERT_CODE_RATE,               /* refused: over the hourly cap */
    ALERT_CODE_COMMAND_EXPIRED,    /* command dropped past its deadline */
    ALERT_CODE_CLOSE_UNCONFIRMED,  /* safety close sent but not confirmed */
    ALERT_CODE_QUEUE_FULL,         /* actor_request() refused: command queue full */
    ALERT_CODE_COMMAND_EVICTED,    /* command bumped from the queue by a safety close */
    ALERT_CODE_COMMAND_FAILED,     /* dispatched, but never reached the actuator: the
                                     * connection, the write or the confirm failed (M5b
                                     * Task 8). Appended, never renumbered -- see this
                                     * enum's own comment. A command that COMPLETED
                                     * UNCONFIRMED is not this: that is an outcome with
                                     * its own policy (spec section 4.4), not a failure. */
    ALERT_CODE_CLOSE_DEVICE_UNREACHABLE, /* a pending close (M5b Task 9) has never once
                                     * been able to reach its device -- not "we tried and
                                     * it refused" (that is ALERT_CODE_CLOSE_UNCONFIRMED,
                                     * and CRITICAL), this is the weaker "we have not been
                                     * able to try at all", posted at EVENT_LEVEL_ALERT,
                                     * exactly once per continuous unreachable streak, so
                                     * an actuator that opened next to a hub that can't
                                     * yet see it doesn't stay open in total silence. */
} alert_code_t;

#define ALERT_RING_LEN 8

/* `repeat` (M5b Task 11, carrying Task 10's Ruling T10-alertchurn): because
 * an activation is recorded at DISPATCH rather than enqueue (Task 7 fix
 * round 2), four rule commands against a max_per_hour=1 pair emit THREE
 * refusal alerts in a burst -- and ALERT_RING_LEN is only 8, the operator's
 * entire visibility surface, so a burst like that can evict everything else
 * in the ring. alert_ring_push() (alert_ring.c, pure) answers this by
 * collapsing CONSECUTIVE pushes whose (code, dev_idx, action_id) match the
 * ring's most-recently-pushed entry into that ONE entry, incrementing
 * `repeat` instead of consuming a new slot -- source-agnostic (a mashed
 * manual button collapses exactly the same way) and needs no guard state.
 * 1 on a genuinely new entry (not "0 more than itself" -- see
 * alert_ring_push()), N on one that collapsed N-1 further identical pushes.
 * level/param always reflect the MOST RECENT push in the run, not the
 * first -- see alert_ring_push()'s own doc comment. */
typedef struct {
    uint8_t  level;
    uint8_t  code;         /* alert_code_t */
    int8_t   dev_idx;
    uint8_t  action_id;    /* ACTION_NONE (0xFF) when this alert is not about
                             * a specific action -- see alert_post()'s comment */
    uint16_t param;
    uint16_t repeat;
} alert_rec_t;

/* ---------------------------------------------------------------------
 * Pure ring (M5b Task 11) -- no FreeRTOS, directly host-tested by
 * tests/host/test_alert_ring.c, the same pure/impure split this component
 * already uses for actor_table.c/actor.c and event_ring.c/event_log.c.
 * alert.c's alert_post()/alert_drain() are the impure front door: take the
 * spinlock, call these, release it.
 * --------------------------------------------------------------------- */

typedef struct {
    alert_rec_t recs[ALERT_RING_LEN];
    uint8_t     head;      /* next slot to write */
    uint8_t     count;     /* valid records queued, 0..ALERT_RING_LEN */
    uint32_t    dropped;   /* oldest-dropped-on-overflow, since last drain */
} alert_ring_t;

void alert_ring_init(alert_ring_t *r);

/* Pushes `in` into `r`. When `r` is non-empty AND its most-recently-pushed
 * entry has the same (code, dev_idx, action_id) as `in`, this COLLAPSES
 * the push into that entry instead of consuming a new slot: `repeat`
 * increments (saturating at UINT16_MAX, never wrapping) and level/param
 * are overwritten with `in`'s -- the latest occurrence's own severity and
 * parameter are what "how bad is this right now" wants to show, not the
 * first one's. Otherwise behaves exactly like the ring did before this
 * field existed: appends a new record (repeat starts at 1, not 0 -- a
 * fresh alert already happened once), evicting the oldest and counting it
 * in `dropped` once the ring is full. `in->repeat` is ignored on input. */
void alert_ring_push(alert_ring_t *r, const alert_rec_t *in);

/* Producers -- the NimBLE host task, an esp_timer callback, the httpd task
 * -- only append a fixed-size record. Cheap and shallow: nothing that
 * touches the radio may touch flash.
 *
 * Pass ACTION_NONE (0xFF, action.h) for `action_id` when the alert is not
 * about a specific action (e.g. a device-level fault) -- action_id 0 is a
 * real action (ACT_SWITCH_ON) and would render as one in the drained
 * message, a wrong, plausible-looking detail in a safety alert.
 * alert_drain() omits the action clause entirely when it sees ACTION_NONE.
 *
 * alert_drain() is called by the rules engine task, which is the ONLY task
 * with the stack for event_log_append's LittleFS + SSE + MQTT chain (8192
 * B, measured 1636 B free). M5a had to cut its event-log write entirely
 * because the decoder task's 3072 B could not hold that chain -- this is
 * the same constraint answered by routing rather than by a new task, which
 * the heap budget could not afford anyway. */
void alert_post(uint8_t level, uint8_t code, int dev_idx, uint8_t action_id, uint16_t param);
void alert_drain(void);

/* Registered once by the rules engine at rules_init() so alert_post() can
 * wake that task immediately, instead of waiting for its next natural wake
 * (a sensor value update or a periodic `every` timer) -- a hub that just
 * raised a safety alert may have neither pending. NULL until registered
 * (or if rules_init() never ran, or itself failed): alert_post() must
 * treat that as a safe no-op, not an error -- see alert_post()'s
 * implementation. */
typedef void (*alert_wake_fn_t)(void);
void alert_set_wake_hook(alert_wake_fn_t fn);
