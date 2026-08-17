#pragma once
/* gatt_sched.h -- the M5a GATT read path's handle cache and per-device read
 * scheduler (design spec docs/superpowers/specs/2026-08-17-planthub-v2-m5a-
 * gatt-read-design.md sections 4 and 5). Pure bookkeeping either side of
 * an actual connection attempt: which GATT handles a device resolved to
 * last time (section 4), and whether a device is due another read yet,
 * with backoff on consecutive failures (section 5). No ESP-IDF, no
 * NimBLE, no timers, no I/O and no time source of its own -- every
 * function here takes `now_s` as a parameter, exactly like gatt_fsm.h's
 * own discipline, because this module is provable on the host and the
 * radio adapter (a later task) is not.
 *
 * Both tables are static module state, keyed by registry slot index
 * (dev_idx), not by a caller-owned struct -- same shape as
 * unknown_capture.c's s_tbl, chosen for the same reason: Task 6's adapter
 * calls these once per connection attempt from a single task, so there is
 * exactly one writer and no benefit to threading a struct pointer through
 * every call. gatt_cache_reset()/gatt_sched_reset() exist for host test
 * isolation between cases (and for a fresh boot) -- Task 6 does not call
 * them itself. */
#include <stdint.h>
#include <stdbool.h>

/* REGISTRY_MAX_DEVICES (planthubos/components/data_core/include/registry.h)
 * is 16 -- restated here, not included, so this module stays
 * dependency-free (this component has no REQUIRES on data_core). Same
 * discipline gatt_fsm.h's GATT_FSM_MAX_READS/GATT_FSM_MAX_WRITES/etc.
 * already hold themselves to against psvm.h, and psvm.h's own
 * ADV_PAYLOAD_MAX-restating precedent (see unknown_capture.h). If
 * registry.h's REGISTRY_MAX_DEVICES ever changes, this must change with
 * it by hand -- nothing in the build enforces that automatically. */
#define GATT_SCHED_MAX_DEVICES 16

/* A plan declares at most 4 reads (psvm.h's PSVM_PLAN_MAX_READS / gatt_fsm.h's
 * GATT_FSM_MAX_READS) -- the cache is sized to exactly that, one slot per
 * possible read. A 5th distinct uuid16 for one device is a bug upstream
 * (a plan that never should have compiled), not a cache-eviction case --
 * gatt_cache_store() refuses it rather than silently dropping a handle a
 * plan still needs. */
#define GATT_CACHE_MAX_ENTRIES 4

/* Consecutive-failure backoff cap: the effective read interval never
 * exceeds this many times the plan's declared interval (design spec
 * section 5). Doubles per consecutive failure (1x, 2x, 4x, 8x, 8x, ...)
 * so a device that goes quiet stops costing radio time, but never drifts
 * to an interval so long a recovered device would look permanently dead. */
#define GATT_SCHED_BACKOFF_CAP 8

/* ---------------- handle cache (design spec section 4) ----------------
 * Handle value 0 means "not cached". Real ATT handles are assigned
 * starting at 1 (handle 0 is reserved by the GATT spec itself), so 0 is a
 * safe sentinel for "gatt_cache_lookup() found nothing" and for "this
 * cache slot is unused" -- gatt_cache_store() must never be called with
 * handle == 0 for a real characteristic. */

/* Returns the cached ATT handle for (dev_idx, uuid16), or 0 if this
 * device has no cached handle for that characteristic (never discovered,
 * or dropped by gatt_cache_drop()). dev_idx outside
 * [0, GATT_SCHED_MAX_DEVICES) returns 0. */
uint16_t gatt_cache_lookup(int dev_idx, uint16_t uuid16);

/* Records that uuid16 resolved to handle for dev_idx (from discovery).
 * Overwrites an existing entry for the same uuid16. If dev_idx's cache
 * already holds GATT_CACHE_MAX_ENTRIES DISTINCT uuid16s and this is a
 * new one, the store is refused (no-op) -- see GATT_CACHE_MAX_ENTRIES'
 * doc comment above for why a 5th entry must never silently evict one of
 * the first four. dev_idx outside [0, GATT_SCHED_MAX_DEVICES) is a no-op. */
void gatt_cache_store(int dev_idx, uint16_t uuid16, uint16_t handle);

/* Clears every cached handle for dev_idx ONLY -- every other device's
 * cache is untouched. Called after a failed read (design spec section 4:
 * a failed read is exactly the event that should invalidate handles,
 * because a firmware update on the device is the thing that moves them).
 * dev_idx outside [0, GATT_SCHED_MAX_DEVICES) is a no-op. */
void gatt_cache_drop(int dev_idx);

/* Clears every device's cache. Test isolation / cold boot; Task 6 does
 * not call this itself. */
void gatt_cache_reset(void);

/* ---------------- read scheduler (design spec section 5) ---------------- */

/* True if dev_idx is due another read at now_s, given the plan's declared
 * interval_s. A device that has never been ATTEMPTED (no gatt_sched_ok()
 * or gatt_sched_fail() call since the last gatt_sched_reset()) is due
 * immediately. Otherwise due = (now_s - <last ATTEMPT time>) >= interval_s
 * x min(2 ^ fail_count, GATT_SCHED_BACKOFF_CAP). Deliberately anchored on
 * the last attempt (gatt_sched_fail()'s last_attempt_s), NOT on
 * gatt_sched_last_ok()'s last SUCCESS -- anchoring on the last success
 * would make a failing device perpetually "past due", so it would retry
 * on every single advertisement instead of backing off, defeating the
 * backoff this function exists to provide (controller ruling, fix round
 * 1: this is why last_ok_s and last_attempt_s are two separate fields,
 * not one). dev_idx outside [0, GATT_SCHED_MAX_DEVICES) returns false. */
bool gatt_sched_due(int dev_idx, uint32_t interval_s, uint32_t now_s);

/* Records a successful read at now_s: clears the failure count to 0 (one
 * success clears backoff completely -- deliberate, see this header's top
 * comment reference to design spec section 5), sets last_attempt_s (so
 * gatt_sched_due()'s backoff anchor advances too) AND sets last_ok_s (so
 * gatt_sched_last_ok() moves forward with it). dev_idx outside range is
 * a no-op. */
void gatt_sched_ok(int dev_idx, uint32_t now_s);

/* Records a failed attempt at now_s: increments the failure count
 * (saturating at 255, never wrapping back to 0 -- a wrap would silently
 * reset backoff exactly like a success does, which a failure must never
 * do) and advances ONLY last_attempt_s, the timestamp gatt_sched_due()'s
 * backoff window is measured from. Deliberately does NOT touch last_ok_s
 * -- design spec section 5 requires a connect block that never succeeds
 * to stay visibly silent (the Devices tab renders the time of the last
 * SUCCESSFUL read, section 8); if a failure advanced last_ok_s too, a
 * device failing every attempt would show "last read: Ns ago" forever,
 * which hides exactly the silence section 5 exists to make loud.
 * dev_idx outside range is a no-op. */
void gatt_sched_fail(int dev_idx, uint32_t now_s);

/* Current consecutive-failure count for dev_idx (0 = last attempt, if
 * any, was a success). dev_idx outside range returns 0. */
uint8_t gatt_sched_fail_count(int dev_idx);

/* Timestamp of dev_idx's most recent SUCCESSFUL gatt_sched_ok() call, or
 * 0 if it has never succeeded (since the last gatt_sched_reset()) --
 * unaffected by any number of gatt_sched_fail() calls in between, even
 * while mid-backoff. Added beyond this task's brief (controller ruling,
 * task-5-brief.md's pre-flight-scan addendum) so Task 7 can render "time
 * since last successful read" in the Devices tab without a second,
 * separately-updated copy of this timestamp -- see this header's module
 * comment. 0 is a safe "never" sentinel for the same reason handle 0 is
 * (this header's handle-cache section, above): now_s is uptime seconds,
 * and a GATT read cannot complete -- radio init, connect, optionally
 * discover, then the read itself all take real time -- at uptime 0, so a
 * real caller's first successful now_s is always > 0. dev_idx outside
 * range returns 0. */
uint32_t gatt_sched_last_ok(int dev_idx);

/* Resets every device's failure count and both timestamps to zero (never
 * attempted, never succeeded). Test isolation / cold boot; Task 6 does
 * not call this itself. */
void gatt_sched_reset(void);
