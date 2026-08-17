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
 * interval_s. A device that has never been contacted (no gatt_sched_ok()
 * or gatt_sched_fail() call since the last gatt_sched_reset()) is due
 * immediately. Otherwise due = (now_s - <last contact time>) >= interval_s
 * x min(2 ^ fail_count, GATT_SCHED_BACKOFF_CAP) -- see gatt_sched_fail()'s
 * doc comment for what "last contact time" means once failures are in
 * play. dev_idx outside [0, GATT_SCHED_MAX_DEVICES) returns false. */
bool gatt_sched_due(int dev_idx, uint32_t interval_s, uint32_t now_s);

/* Records a successful read at now_s: clears the failure count to 0 (one
 * success clears backoff completely -- deliberate, see this header's top
 * comment reference to design spec section 5) and sets the timestamp
 * gatt_sched_last_ok() reports. dev_idx outside range is a no-op. */
void gatt_sched_ok(int dev_idx, uint32_t now_s);

/* Records a failed attempt at now_s: increments the failure count
 * (saturating at 255, never wrapping back to 0 -- a wrap would silently
 * reset backoff exactly like a success does, which a failure must never
 * do) and, like gatt_sched_ok(), advances the timestamp
 * gatt_sched_due()'s backoff window is measured from. This is
 * DELIBERATE and is the one place this module's naming is imprecise: the
 * stored timestamp is really "last contact time, success or failure", not
 * literally "last successful read" -- see gatt_sched_last_ok()'s doc
 * comment for the consequence. There is no second timestamp field to
 * anchor backoff to (the scheduler table is sized to exactly u32+u8+u8 =
 * 6 B/device, 96 B total, per this task's brief); anchoring backoff on
 * anything OTHER than the most recent attempt would make consecutive
 * failures fail to extend the retry window, which breaks convergence.
 * dev_idx outside range is a no-op. */
void gatt_sched_fail(int dev_idx, uint32_t now_s);

/* Current consecutive-failure count for dev_idx (0 = last contact, if
 * any, was a success). dev_idx outside range returns 0. */
uint8_t gatt_sched_fail_count(int dev_idx);

/* Timestamp of dev_idx's most recent gatt_sched_ok() or gatt_sched_fail()
 * call, or 0 if neither has ever been called (since the last
 * gatt_sched_reset()). Added beyond this task's brief (controller ruling,
 * task-5-brief.md's pre-flight-scan addendum) so Task 7 can render "time
 * since last successful read" in the Devices tab without a second,
 * separately-updated copy of this timestamp -- see this header's module
 * comment. CAVEAT the ruling did not anticipate: while
 * gatt_sched_fail_count(dev_idx) > 0, this is the time of the most recent
 * FAILED attempt, not the most recent SUCCESS -- see gatt_sched_fail()'s
 * doc comment for why there is no separate field to keep the two apart.
 * A caller that wants strictly "last successful read, even mid-backoff"
 * cannot get it from this module as specified; it can only get "last
 * contact, whatever the outcome" (fail_count()==0 tells it whether that
 * doubles as a success). dev_idx outside range returns 0. */
uint32_t gatt_sched_last_ok(int dev_idx);

/* Resets every device's failure count and last-contact timestamp to zero
 * (never contacted). Test isolation / cold boot; Task 6 does not call
 * this itself. */
void gatt_sched_reset(void);
