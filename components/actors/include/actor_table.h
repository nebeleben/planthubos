#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "action.h"

/* The safety core's decision logic (spec section 1.4 and section 4.2): which
 * device supports which action, and whether a command is allowed right now.
 * Deliberately pure C, no ESP-IDF, no radio, no I/O -- everything downstream
 * (actor_request(), Task 7) trusts this module's verdict, so it is the one
 * part of the safety core a host test can prove exhaustively.
 *
 * Guards attach to the (device, action) PAIR, not to a plant or a rule: two
 * plants bound to the same valve, plus a manual press, all draw on ONE
 * budget, so "max 4 opens per hour" means the valve opens at most four
 * times total, from any source. That is the whole reason this table exists
 * instead of a per-rule counter. */

#define ACTOR_MAX_DEVICES 4
#define ACTOR_MAX_ACTIONS 4

typedef enum {
    ACTOR_SRC_RULE = 0,
    ACTOR_SRC_MANUAL,
    ACTOR_SRC_SAFETY,
} actor_source_t;

/* actor_table_check()'s verdict. The refusal reason is part of the
 * contract, not just a bool, because it becomes the text a user reads in
 * an alert: "refused, over the hourly cap" is more useful than "refused,
 * unknown" when both happen to be true. */
typedef enum {
    ACTOR_OK = 0,
    ACTOR_REFUSED_UNKNOWN,
    ACTOR_REFUSED_BOUND,
    ACTOR_REFUSED_LOCKOUT,
    ACTOR_REFUSED_COOLDOWN,
    ACTOR_REFUSED_RATE,
} actor_verdict_t;

/* Per (device, action) pair state -- 16 B, pinned below. This hub's static
 * budget already failed once on hardware (spec section 8) and had to be
 * recovered, so this size is load-bearing, not a suggestion.
 *
 * The hourly rate limit is a FIXED window, not a sliding one: a sliding
 * window needs a ring of timestamps per pair, and at ACTOR_MAX_DEVICES *
 * ACTOR_MAX_ACTIONS (16) pairs that is hundreds of bytes this hub does not
 * have. The honest cost is that activations can cluster across a window
 * boundary -- up to 2N in one 60-minute span -- and the cooldown is what
 * bounds how tightly. This is a documented trade, not a bug to "fix" into
 * a sliding window. */
typedef struct {
    uint8_t  action_id;        /* ACTION_NONE (from action.h) when free */
    uint8_t  flags;            /* bit 0 device-local timed-off, bit 1 has
                                 * confirm -- opaque to this module, carried
                                 * through from the wrapper for later tasks */
    uint16_t param_max;        /* effective bound: min(wrapper, firmware) */
    uint16_t cooldown_s;       /* 0 = no cooldown configured */
    uint8_t  max_per_hour;     /* 0 = unlimited */
    uint8_t  window_count;     /* fires seen since window_start_s; also
                                 * doubles as "has this pair ever fired",
                                 * since actor_table_record() always leaves
                                 * it >= 1 -- see actor_table.c */
    uint32_t last_fire_s;
    uint32_t window_start_s;
} actor_slot_t;
_Static_assert(sizeof(actor_slot_t) == 16, "spec section 8 budgets 16 B per (device, action)");

/* One row per known device, holding up to ACTOR_MAX_ACTIONS declared
 * actions for it plus the device's own lockout state (the operator's stop
 * button -- see actor_table_check()'s comment on ordering). dev_idx is a
 * key looked up by linear scan, not a direct array index: the actor table
 * only tracks the (small) subset of devices that have actuator actions,
 * not the whole device registry, so dev_idx values are sparse. -1 marks an
 * unused row. */
typedef struct {
    int          dev_idx;
    bool         lockout;
    actor_slot_t actions[ACTOR_MAX_ACTIONS];
} actor_device_t;

typedef struct {
    actor_device_t devices[ACTOR_MAX_DEVICES];
    uint32_t        full_drops;    /* actor_table_add() calls refused for
                                     * want of a free device row or action
                                     * slot, since the table was populated */
} actor_table_t;

void actor_table_init(actor_table_t *t);

/* Declares that `dev_idx` supports `action_id`, with `param_max` as the
 * WRAPPER's declared bound (tightening the firmware's own hard bound from
 * action.h is always allowed; the effective bound stored is
 * min(param_max, the firmware's action_get(action_id)->param_max)).
 * `flags` is opaque here -- see actor_slot_t's comment.
 *
 * Returns false, and counts the refusal in actor_table_full_drops(), when
 * `action_id` is unknown to action.h, or the table has no room: no free
 * device row for a new dev_idx, or no free action slot in dev_idx's row.
 * Calling again for a pair already declared updates it in place rather
 * than consuming a second slot. */
bool actor_table_add(actor_table_t *t, int dev_idx, uint8_t action_id,
                      uint16_t param_max, uint8_t flags);

/* The single source of truth for "is this command allowed right now".
 * Evaluates in this fixed order and returns the FIRST refusal:
 *
 *   unknown -> bound -> lockout -> cooldown -> rate
 *
 * Order matters for the alert text (see actor_verdict_t's comment) and for
 * safety: BOUND is checked before LOCKOUT so a locked-out device still
 * reports an out-of-range parameter as a bound violation, not a lockout,
 * and LOCKOUT refuses ACTOR_SRC_RULE while permitting ACTOR_SRC_MANUAL and
 * ACTOR_SRC_SAFETY -- a lockout that blocked the safety close would strand
 * an actuator open, which is the opposite of safe.
 *
 * Read-only: does not record a fire. Callers that decide to proceed must
 * call actor_table_record() themselves once the command is actually sent. */
actor_verdict_t actor_table_check(actor_table_t *t, int dev_idx, uint8_t action_id,
                                   uint16_t param, actor_source_t source, uint32_t now_s);

/* Records that (dev_idx, action_id) fired at now_s: updates the cooldown
 * clock and the fixed-window rate counter. A no-op if the pair is not
 * declared. Takes no `source` -- guards are one budget shared by every
 * source, by design (see this file's top comment). */
void actor_table_record(actor_table_t *t, int dev_idx, uint8_t action_id, uint32_t now_s);

/* Configures the cooldown and hourly cap for an already-declared pair.
 * Returns false if the pair is not declared. cooldown_s == 0 disables the
 * cooldown; max_per_hour == 0 disables the rate cap ("unlimited"). */
bool actor_table_set_guards(actor_table_t *t, int dev_idx, uint8_t action_id,
                             uint16_t cooldown_s, uint8_t max_per_hour);

/* Sets or clears the device-level lockout (a no-op if dev_idx is not
 * declared). See actor_table_check()'s comment for what lockout does and
 * does not refuse. */
void actor_table_set_lockout(actor_table_t *t, int dev_idx, bool on);

uint32_t actor_table_full_drops(const actor_table_t *t);
