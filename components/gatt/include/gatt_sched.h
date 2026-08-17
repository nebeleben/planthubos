#pragma once
/* gatt_sched.h -- the M5a GATT read path's per-device read scheduler
 * (design spec docs/superpowers/specs/2026-08-17-planthub-v2-m5a-
 * gatt-read-design.md section 5). Pure bookkeeping either side of an
 * actual connection attempt: whether a device is due another read yet,
 * with backoff on consecutive failures. No ESP-IDF, no NimBLE, no timers,
 * no I/O and no time source of its own -- every function here takes
 * `now_s` as a parameter, exactly like gatt_fsm.h's own discipline,
 * because this module is provable on the host and the radio adapter (a
 * later task) is not.
 *
 * This used to also own a per-device GATT handle cache (design spec
 * section 4, original wording), removed during the M5a hardware gate: the
 * bench found a stale cached handle can point at a DIFFERENT attribute
 * after the peer's attribute table shifts (a declaration, which is always
 * readable) and the read then "succeeds" with the wrong bytes decoded as
 * plausible garbage -- a failure section 5's own invalidation rule cannot
 * catch, because the read never fails. Every read and write is now
 * addressed by uuid16 on every connection instead (gatt_engine.c resolves
 * it via NimBLE's read-by-UUID procedure, server-side, each time), so
 * there is nothing left for a handle cache to do. See gatt_fsm.h's
 * gatt_fsm_init() doc comment for the same story from the state machine's
 * side.
 *
 * The table below is static module state, keyed by registry slot index
 * (dev_idx), not by a caller-owned struct -- same shape as
 * unknown_capture.c's s_tbl, chosen for the same reason: there is no
 * benefit to threading a struct pointer through every call when there is
 * exactly one of it.
 *
 * CONCURRENCY (corrected in Task 6 fix round 1; the original wording here
 * claimed a single TASK, which is no longer true and a false invariant is
 * worse than none). The WRITER is the NimBLE host task --
 * gatt_sched_ok()/fail()/attempt() are called only from gatt_engine.c's
 * callbacks and npl handlers, which all run there. The READERS are spread:
 * gatt_sched_due() is called from adv_decoder_task (ble_collector.c's
 * per-advertisement trigger), and Task 7's httpd handler calls
 * gatt_sched_last_ok()/fail_count() from a third task. That
 * single-writer/multi-reader shape needs no lock on this target: every
 * field read across a task boundary is a naturally-aligned u32 or u8, so a
 * reader can never see a torn value -- only, at worst, one field of an
 * attempt's bookkeeping updated a few instructions before another, costing
 * one mistimed due() decision on a device that is about to be
 * re-evaluated on its next advertisement anyway. Anything added here that
 * needed a multi-word invariant to hold across tasks would need a real
 * lock.
 *
 * gatt_sched_reset() exists for host test isolation between cases (and for
 * a fresh boot) -- Task 6 does not call it itself. */
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

/* Consecutive-failure backoff cap: the effective read interval never
 * exceeds this many times the plan's declared interval (design spec
 * section 5). Doubles per consecutive failure (1x, 2x, 4x, 8x, 8x, ...)
 * so a device that goes quiet stops costing radio time, but never drifts
 * to an interval so long a recovered device would look permanently dead. */
#define GATT_SCHED_BACKOFF_CAP 8

/* ---------------- read scheduler (design spec section 5) ---------------- */

/* True if dev_idx is due another read at now_s, given the plan's declared
 * interval_s. A device that has never been ATTEMPTED (no gatt_sched_ok()
 * or gatt_sched_fail() call since the last gatt_sched_reset()) is due
 * immediately -- this is tracked with an explicit internal "attempted"
 * flag, NOT inferred from last_attempt_s being zero (fix round 2,
 * controller ruling: now_s == 0 is a real, reachable value -- an
 * immediate NimBLE rejection needs no radio round-trip, and uptime is
 * 1-second resolution, so the first second after boot is a real window --
 * so a zero TIMESTAMP can legitimately mean "attempted, at t=0", and
 * inferring "never" from it would silently skip backoff on exactly that
 * attempt). Otherwise due = (now_s - <last ATTEMPT time>) >= interval_s x
 * min(2 ^ fail_count, GATT_SCHED_BACKOFF_CAP). Deliberately anchored on
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
 * gatt_sched_due()'s backoff anchor advances too), sets last_ok_s (so
 * gatt_sched_last_ok() moves forward with it), and marks both internal
 * "attempted" and "has ever succeeded" flags true -- explicitly, not by
 * the timestamps becoming nonzero, because now_s == 0 is a real value a
 * success can legitimately land on (fix round 2; see gatt_sched_due()'s
 * doc comment for why the same argument applies here). dev_idx outside
 * range is a no-op. */
void gatt_sched_ok(int dev_idx, uint32_t now_s);

/* Records a failed attempt at now_s: increments the failure count
 * (saturating at 255, never wrapping back to 0 -- a wrap would silently
 * reset backoff exactly like a success does, which a failure must never
 * do), advances ONLY last_attempt_s (the timestamp gatt_sched_due()'s
 * backoff window is measured from), and marks the internal "attempted"
 * flag true -- even when now_s is 0 (fix round 2: a first contact that
 * fails within the first second of uptime must still get a real backoff
 * window, not be read back as "never attempted"). Deliberately does NOT
 * touch last_ok_s or the "has ever succeeded" flag -- design spec section
 * 5 requires a connect block that never succeeds to stay visibly silent
 * (the Devices tab renders the time of the last SUCCESSFUL read, section
 * 8); if a failure advanced last_ok_s too, a device failing every attempt
 * would show "last read: Ns ago" forever, which hides exactly the silence
 * section 5 exists to make loud. dev_idx outside range is a no-op. */
void gatt_sched_fail(int dev_idx, uint32_t now_s);

/* Records an attempt whose RADIO WORK SUCCEEDED but which produced no
 * value: every declared read landed and the decode ran, and the decode
 * emitted nothing (Task 6 fix round 1, controller ruling). A third outcome
 * is needed because neither existing one is honest about this case:
 *
 *   - gatt_sched_ok() would advance last_ok_s, so section 8's Devices tab
 *     would render the device as freshly read while it contributes nothing
 *     to history -- exactly the silence section 5 exists to make loud, and
 *     since the event-log entry was cut from M5a (see the amended section
 *     5), the last-successful-read timestamp and last_error ARE the whole
 *     visibility surface. Advancing it would hide the one thing left.
 *   - gatt_sched_fail() would back the device off, punishing a device whose
 *     radio behaviour was faultless for a decision its wrapper's `require`
 *     made.
 *
 * So this advances last_attempt_s (the interval gate must move, or the hub
 * would reconnect on the very next advertisement forever) and marks
 * `attempted`, CLEARS the consecutive-failure count exactly as
 * gatt_sched_ok() does -- consecutive RADIO failures are demonstrably over,
 * and leaving an 8x backoff on a device that answers every read would be
 * backing off the wrong thing -- and leaves last_ok_s and the "has ever
 * succeeded" flag untouched, exactly as gatt_sched_fail() does. dev_idx
 * outside range is a no-op. */
void gatt_sched_attempt(int dev_idx, uint32_t now_s);

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
 * comment. "Never succeeded" is tracked with an explicit internal flag,
 * NOT inferred from last_ok_s being zero (fix round 2: a real success can
 * legitimately land on now_s == 0, exactly like gatt_sched_due()'s
 * "attempted" flag above -- a zero RETURN VALUE from this function is
 * therefore not itself proof of "never"; gatt_sched_due()'s behaviour,
 * which uses the flag directly, is what actually distinguishes the two).
 * dev_idx outside range returns 0. */
uint32_t gatt_sched_last_ok(int dev_idx);

/* Resets every device's failure count and both timestamps to zero (never
 * attempted, never succeeded). Test isolation / cold boot; Task 6 does
 * not call this itself. */
void gatt_sched_reset(void);
