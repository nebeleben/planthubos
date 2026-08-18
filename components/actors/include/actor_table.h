#pragma once
#include <stdbool.h>
#include <stddef.h>
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

/* A device's STABLE identity, as opposed to its registry index. Exactly
 * sizeof(device_id_t) (capability.h: `{ uint8_t kind; uint8_t addr[8]; }`),
 * carried here as opaque bytes so this module keeps no dependency on the
 * registry -- the caller that declares an actuator supplies it
 * (actor_table_set_key()).
 *
 * It exists because a registry index is NOT stable across a reboot: the
 * registry is RAM-only (data_core.c's `static registry_t s_registry`) and
 * slots are claimed in discovery order, so the valve that was device 2
 * yesterday may be device 0 today. Anything persisted to flash and matched
 * back to a device after a restart must therefore key on this, never on
 * dev_idx -- see actor_persist.h. All-zero means "no key set", which is not
 * a real device_id_t (kind 0 with an all-zero address is not a device this
 * hub can address) and marks a row that must not be persisted. */
#define ACTOR_DEVICE_KEY_LEN 9

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
/* M5b Task 9: named bit for actor_slot_t.flags bit 0 -- this table stores
 * the bit opaquely (it is carried straight through from the wrapper's
 * action-table flags byte, psvm.h's PSVM_FLAG_ACTION_TABLE layout), but
 * Task 9 is the first reader that has to ACT on it: an action with this bit
 * set closes itself on the device after its duration elapses, so the hub
 * must not (and, per pending_close_arm()'s contract, must not) schedule a
 * close for it. An action without it is the hub's obligation. */
#define ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF 0x01u

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
    /* Whole-branch review, ruling FINAL-persist: the stable identity this
     * row's guards are persisted under. Placed here, between two fields
     * that already leave three bytes of alignment padding before
     * `actions` needs 4-byte alignment, so it costs 8 B per device row
     * (72 -> 80 B), 32 B across the table. */
    uint8_t      key[ACTOR_DEVICE_KEY_LEN];
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
 * Returns false when `dev_idx` is negative, `action_id` is unknown to
 * action.h, or the pair cannot be added for either reason. It ALSO
 * returns false, additionally counting the refusal in
 * actor_table_full_drops(), specifically when the table has no room: no
 * free device row for a new dev_idx, or no free action slot in dev_idx's
 * row. The two false-return cases are deliberately not folded into one
 * counter -- a caller trying to diagnose "why won't this add" needs to
 * tell "bad input" from "the table is full" apart.
 *
 * Calling again for a pair already declared updates `param_max` and
 * `flags` in place rather than consuming a second slot, and leaves that
 * pair's cooldown, rate cap and window state untouched -- a wrapper
 * re-parse, a re-discovery pass or an API-driven wrapper update must not
 * reset an operator's guards or erase an hourly budget already spent.
 * (The device row's `lockout` already survives a re-add the same way; a
 * new row starts with lockout off, same as a genuinely new pair starts
 * with clean guards.) */
bool actor_table_add(actor_table_t *t, int dev_idx, uint8_t action_id,
                      uint16_t param_max, uint8_t flags);

/* The single source of truth for "is this command allowed right now".
 * Evaluates in this fixed order and returns the FIRST refusal:
 *
 *   unknown -> bound -> lockout -> cooldown -> rate
 *
 * Order matters for the alert text (see actor_verdict_t's comment) and for
 * safety. Per-guard, by source, this is the module's contract:
 *
 *   unknown  -> refuses every source (dev_idx < 0 counts as unknown)
 *   bound    -> refuses every source, ACTOR_SRC_SAFETY included -- it is a
 *               correctness check, not a rate limit, and a close carries
 *               no parameter, so it never blocks a legitimate close
 *   lockout  -> refuses ACTOR_SRC_RULE; permits MANUAL and SAFETY
 *   cooldown -> refuses RULE and MANUAL; permits SAFETY
 *   rate     -> refuses RULE and MANUAL; permits SAFETY
 *
 * The principle behind the last three: ACTOR_SRC_SAFETY is exempt from
 * every rate-shaping guard (lockout, cooldown, rate), because its job is
 * to close something already open -- a lockout, a close-retry storm, or a
 * user-configured cooldown/rate cap on switch.off must never be able to
 * strand an actuator open, which is the opposite of safe.
 *
 * Read-only: does not record a fire. Callers that decide to proceed must
 * call actor_table_record() themselves once the command is actually sent. */
actor_verdict_t actor_table_check(actor_table_t *t, int dev_idx, uint8_t action_id,
                                   uint16_t param, actor_source_t source, uint32_t now_s);

/* Records that (dev_idx, action_id) fired at now_s: updates the cooldown
 * clock and the fixed-window rate counter. A no-op if dev_idx is negative
 * or the pair is not declared. Takes no `source` -- guards are one budget
 * shared by every source, by design (see this file's top comment). */
void actor_table_record(actor_table_t *t, int dev_idx, uint8_t action_id, uint32_t now_s);

/* Configures the cooldown and hourly cap for an already-declared pair.
 * Returns false if dev_idx is negative or the pair is not declared.
 * cooldown_s == 0 disables the cooldown; max_per_hour == 0 disables the
 * rate cap ("unlimited"). */
bool actor_table_set_guards(actor_table_t *t, int dev_idx, uint8_t action_id,
                             uint16_t cooldown_s, uint8_t max_per_hour);

/* Sets or clears the device-level lockout (a no-op if dev_idx is negative
 * or not declared). See actor_table_check()'s comment for what lockout
 * does and does not refuse. */
void actor_table_set_lockout(actor_table_t *t, int dev_idx, bool on);

/* Undeclares dev_idx entirely: the row is freed, so every action it
 * declared, their guard configuration and their spent hourly budget, and
 * the device's lockout, are all gone. Returns false when dev_idx was not
 * declared (nothing to remove).
 *
 * The inverse of actor_table_add(), and deliberately a much blunter
 * instrument than it: add() re-declares in place precisely so a wrapper
 * re-parse never resets an operator's guards, so removal must be something
 * a caller asks for explicitly, on evidence, and never a routine step of
 * re-binding. M5b Task 8 fix round 1 has the one caller and the one piece
 * of evidence that justifies it: ble_collector.c's wrapper reindex, for a
 * bound actuator whose wrapper NO LONGER declares any action -- the
 * wrapper was deleted, or its action block was. Without it that device
 * stays declared forever, every command a rule queues for it passes the
 * guards, reaches dispatch, fails for want of a wrapper and alerts --
 * indefinitely, for a device the operator removed.
 *
 * A device whose wrapper still declares actions is NOT removed and must
 * not be: it would cost that operator their cooldown, their hourly cap and
 * their lockout every time any unrelated wrapper was installed. */
bool actor_table_remove(actor_table_t *t, int dev_idx);

uint32_t actor_table_full_drops(const actor_table_t *t);

/* M5b Task 9: read-only accessor for a declared pair's stored `flags` (see
 * ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF above) -- the one thing pending_close
 * needs from this table and the only reason it looks here at all. Returns
 * false (and leaves *flags_out untouched) when dev_idx is negative or the
 * pair is not declared, exactly like actor_table_set_guards()'s contract. */
bool actor_table_action_flags(const actor_table_t *t, int dev_idx, uint8_t action_id,
                               uint8_t *flags_out);

/* ---------------------------------------------------------------------
 * M5b Task 11: read-only accessors for the HTTP API. Task 7 fix round 1
 * (finding 4) deliberately removed the one thing that would have made
 * these trivial -- a raw actor_table_get() pointer -- because handing the
 * httpd task an unlocked pointer let it race the actor task inside
 * actor_table_check() (a torn read of a slot mid-actor_table_record()
 * write). These stay read-only and are always called through actor.c's
 * lock-taking wrappers (actor_pair_state()/actor_lockout()), the same
 * discipline actor_declare()/actor_configure_guards()/actor_set_lockout()
 * already hold callers to.
 * --------------------------------------------------------------------- */

/* Everything the API's per-action JSON needs about one declared pair, in
 * one snapshot so a single lock/unlock produces a consistent read (rather
 * than the caller re-taking the lock once per field and possibly
 * observing a guard change land in between).
 *
 *   param_max              the EFFECTIVE bound (actor_table_add()'s own
 *                           min(wrapper, firmware) contract) -- what
 *                           actor_table_check() actually enforces, not
 *                           action.h's raw ceiling.
 *   activations_this_hour  window_count iff the current hourly window is
 *                           still open (window_still_open(), actor_table.c
 *                           -- the exact same test actor_table_check()'s
 *                           own rate guard uses, so the two can never
 *                           disagree about whether a count is stale), 0
 *                           otherwise.
 *   has_fired/last_fire_s  whether this pair has EVER recorded a fire (not
 *                           just within the current window) and, iff so,
 *                           when -- window_count > 0 doubles for "ever
 *                           fired" exactly as actor_table_check()'s own
 *                           cooldown check already relies on.
 *   live_verdict           what actor_table_check() would return RIGHT
 *                           NOW for a MANUAL invocation of this pair,
 *                           computed using this slot's own param_max as
 *                           the synthetic parameter -- action_param_ok()
 *                           always accepts a slot's own effective bound
 *                           (0 when the action takes no parameter, the
 *                           bound itself otherwise), so BOUND can never
 *                           spuriously appear here. Lockout never appears
 *                           either: it refuses RULE only (actor_table.h's
 *                           module contract) and permits MANUAL, which is
 *                           exactly what this reports -- the JSON's
 *                           separate "lockout" field is what tells an
 *                           operator a device is stopped; this field
 *                           tells them whether THEIR press would go
 *                           through right now. Only ACTOR_OK,
 *                           ACTOR_REFUSED_COOLDOWN or ACTOR_REFUSED_RATE
 *                           are possible outcomes. */
typedef struct {
    uint16_t        param_max;
    uint16_t        cooldown_s;
    uint8_t         max_per_hour;
    uint8_t         activations_this_hour;
    bool            has_fired;
    uint32_t        last_fire_s;
    actor_verdict_t live_verdict;
} actor_pair_state_t;

/* Returns false (leaves *out untouched) when dev_idx is negative or the
 * pair is not declared -- same not-declared contract as
 * actor_table_action_flags(). `t` is const: this never writes, unlike
 * actor_table_check() (which takes a mutable pointer only because its
 * find_row()/find_slot() helpers are shared with the mutating calls in
 * this file). */
bool actor_table_pair_state(const actor_table_t *t, int dev_idx, uint8_t action_id,
                             uint32_t now_s, actor_pair_state_t *out);

/* Read-only device-level lockout (the operator's stop button,
 * actor_table_check()'s own comment) for the HTTP API. Returns false
 * (leaves *out untouched) when dev_idx is negative or the device has no
 * declared actions at all -- same not-declared contract as every other
 * accessor here. */
bool actor_table_lockout(const actor_table_t *t, int dev_idx, bool *out);

/* ---------------------------------------------------------------------
 * Whole-branch review, ruling FINAL-persist: guards across a reboot.
 *
 * This table was RAM-only, so after an OTA, a brownout or a power cycle
 * the operator's LOCKOUT came up OFF, every cooldown came up 0 and every
 * hourly cap came up unlimited -- and ble_collector.c sets s_actors_wired
 * long before an operator could re-set any of them, so the first sensor
 * update could fire a rule into an unlocked actuator. A reboot LOOP reset
 * the flood counter every cycle, which directly negates spec section
 * 4.2's "max 4 opens per hour means the valve opens at most four times".
 *
 * The primitives below are the pure half; actor_persist.h owns the file.
 * --------------------------------------------------------------------- */

/* Records the stable identity of an already-declared device. A no-op when
 * dev_idx is negative or not declared, or when `key` is NULL. Called once
 * per device by whoever declares it (ble_collector.c), from the same
 * advertisement that resolved the registry index. */
void actor_table_set_key(actor_table_t *t, int dev_idx,
                          const uint8_t key[ACTOR_DEVICE_KEY_LEN]);

/* Reads back a declared device's stable identity. False (leaving *out
 * untouched) when dev_idx is negative, not declared, or has no key set. */
bool actor_table_device_key(const actor_table_t *t, int dev_idx,
                             uint8_t out[ACTOR_DEVICE_KEY_LEN]);

/* The inverse: the registry index of the DECLARED device carrying `key`,
 * or -1. This is the resolution step anything persisted must go through,
 * and it is deliberately resolved at the moment of use rather than once at
 * load -- a device that has not advertised yet this boot is simply not
 * here, and "not yet" must read as -1 (defer) rather than as some other
 * device's index. A row with no key set can never match, so the all-zero
 * sentinel cannot alias a real device. */
int  actor_table_find_by_key(const actor_table_t *t,
                              const uint8_t key[ACTOR_DEVICE_KEY_LEN]);

/* One persisted (device, action) pair: the guard CONFIG an operator set
 * plus the guard STATE that says how much of the budget is already spent.
 * 24 B, and ACTOR_MAX_DEVICES * ACTOR_MAX_ACTIONS (16) of them is the most
 * that can ever exist.
 *
 * `lockout` is device-level (actor_device_t), replicated into every one of
 * that device's rows: the rows are always written from one snapshot, so
 * they cannot disagree, and it saves a second nested record type in the
 * file -- which this milestone's own review history says is the right
 * trade (a flat, exact-length record is what the validator can be trusted
 * about).
 *
 * `window_start_s` and `last_fire_s` are UPTIME seconds (actor_now_s()),
 * so they are meaningless across a restart -- uptime restarts at zero.
 * They are written faithfully so the file is a true record of the state at
 * save time, and deliberately RE-BASED, never believed, on load: see
 * actor_table_guard_apply(). */
typedef struct {
    uint32_t window_start_s;
    uint32_t last_fire_s;
    uint16_t cooldown_s;
    uint8_t  key[ACTOR_DEVICE_KEY_LEN];
    uint8_t  action_id;
    uint8_t  max_per_hour;
    uint8_t  window_count;
    bool     lockout;
} actor_guard_row_t;
_Static_assert(sizeof(actor_guard_row_t) == 24,
               "ruling FINAL-persist budgets 24 B per persisted guard row");

#define ACTOR_GUARD_ROWS_MAX (ACTOR_MAX_DEVICES * ACTOR_MAX_ACTIONS)

/* Folds the live table into `rows` -- the image last written to flash --
 * IN PLACE, and returns the new row count. Pure: no clock, no I/O.
 *
 * Three rules, in this order:
 *
 *   1. A row whose key belongs to a device that IS declared right now, for
 *      an action that device no longer declares, is dropped. (The wrapper's
 *      action block changed, or actor_table_remove() ran.)
 *   2. Every declared pair with a key is written over the row with the
 *      same (key, action_id), or into a free slot, or -- only when `cap` is
 *      exhausted -- over the first row whose key belongs to NO currently
 *      declared device. A live pair therefore always gets a slot: `cap` is
 *      ACTOR_GUARD_ROWS_MAX, which is exactly the number of pairs that can
 *      be live at once, so an eviction candidate provably exists whenever
 *      one is needed.
 *   3. Every OTHER row is left exactly as it was. This is the load-bearing
 *      one: a valve that has not advertised since the last boot is not
 *      declared, and its operator's lockout must not be erased from flash
 *      just because some other device fired. A device that never comes
 *      back keeps its row until rule 2 needs the slot.
 *
 * Rule 3 covers actor_table_remove() too, and deliberately: removing a
 * device empties its LIVE guards (that function's own contract) but leaves
 * its persisted ones, so reinstalling the wrapper an operator deleted
 * gives them their lockout and cooldown back rather than silently
 * unlocking the valve. Rows for devices that never return are bounded by
 * rule 2's eviction, so they cannot accumulate.
 *
 * A declared device with no key (actor_table_set_key() never called) is
 * skipped by rules 1 and 2 alike -- there is nothing to match it back to
 * after a reboot, so persisting it would be worse than not. */
size_t actor_table_guard_merge(const actor_table_t *t, actor_guard_row_t *rows,
                                size_t n, size_t cap);

/* Applies one restored row to an already-declared pair. Returns false
 * (changing nothing) when dev_idx is negative, the device is not declared,
 * or it does not declare row->action_id.
 *
 * THE TIMESTAMP DECISION, stated here because it is a safety choice and
 * not an implementation detail: `window_start_s` and `last_fire_s` are
 * uptime, and uptime restarts, so the file's values cannot be used as
 * clock readings. Both are set to `now_s` -- the instant of the restore --
 * while `window_count` is restored VERBATIM. That means:
 *
 *   - the hourly budget is NOT refunded by a reboot. A valve that has
 *     spent 4 of 4 opens comes back having spent 4 of 4.
 *   - the restored window is treated as still open, and stays open for a
 *     full ACTOR_WINDOW_S from the restore. That is the most restrictive
 *     honest reading: the alternative (crediting the elapsed time recorded
 *     before the reboot) would hand back budget on evidence a crashed hub
 *     cannot vouch for, and a reboot loop is exactly the case where that
 *     matters.
 *   - the cooldown likewise restarts in full rather than resuming.
 *
 * Both errors are in the direction of refusing a command that might have
 * been allowed, never of allowing one that should have been refused. */
bool actor_table_guard_apply(actor_table_t *t, int dev_idx,
                              const actor_guard_row_t *row, uint32_t now_s);
