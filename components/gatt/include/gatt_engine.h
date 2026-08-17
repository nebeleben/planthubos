#pragma once
/* gatt_engine.h -- the NimBLE adapter for M5a's GATT read path (design spec
 * docs/superpowers/specs/2026-08-17-planthub-v2-m5a-gatt-read-design.md
 * sections 3, 4 and 5). This is the ONE module in M5a that cannot be
 * host-tested: gatt_fsm.c (which sequence to run, and what every failure
 * path does) and gatt_sched.c (whether a device is due, and how a failure
 * backs it off) were deliberately built pure so that all of that IS proved
 * by execution on the host. What is left here is translation only --
 * "the state machine decides, this file talks to the radio" -- and its
 * correctness rests on the hardware gate rather than on a suite.
 *
 * Nothing in this file makes a decision gatt_fsm.c could have made. The
 * places where it genuinely had to (the ATT-handle/uuid16 mapping, which
 * discovered characteristics are worth caching, what counts as a
 * "successful" attempt, and which task each half of the work runs on) are
 * called out in gatt_engine.c's own comments at the exact point they are
 * made, because those are precisely what the hardware gate has to probe.
 *
 * TASK OWNERSHIP (the single most load-bearing property of this file):
 *
 *   NimBLE host task  -- the SOLE WRITER of gatt_fsm_t, of every field
 *                        gatt_engine.c uses to drive an attempt, and of
 *                        gatt_sched.c's tables. Every gatt_fsm_step() in
 *                        this codebase happens here, so the state machine
 *                        needs no lock. The GAP/GATT callbacks already run
 *                        here; the two things that do not (a request from
 *                        the decoder task, and the deadline esp_timer) are
 *                        funnelled onto it as NimBLE ble_npl events rather
 *                        than touching that state from their own task.
 *   adv_decoder_task  -- owns everything that touches flash or runs the VM:
 *                        creating a request (which copies the plan out of
 *                        the wrapper arena) and running the decode. Neither
 *                        may happen on the NimBLE host task (M3 spec
 *                        section 1), and the connection must not be driven
 *                        from the decoder task (a connect attempt is
 *                        hundreds of milliseconds of radio work and that
 *                        task must keep draining the advertisement ring).
 *                        GS_DECODING -- the state gatt_fsm.h added so the
 *                        success path also emits exactly one GA_DISCONNECT
 *                        -- is exactly the seam where the work crosses back
 *                        over.
 *
 * It is a single-writer, multiple-reader arrangement, NOT single-task:
 * adv_decoder_task reads engine state through gatt_engine_busy() and reads
 * gatt_sched_due(), and Task 7's httpd handler will read
 * gatt_engine_last_error()/gatt_sched_last_ok() from a third task. All of
 * those reads are of naturally-aligned scalars or of pointers to string
 * literals on a single-core target, so the worst case is a reader observing
 * one field of an attempt's bookkeeping updated slightly before another and
 * making one mistimed scheduling decision -- never a torn value and never a
 * dangling pointer. No reader ever writes. Anything added here that needs a
 * multi-word invariant across tasks would need a lock, which today nothing
 * does.
 *
 * No new FreeRTOS task (spec section 3's "no new task, no blocking call"),
 * which is also what keeps this inside the section 6 memory budget: a task
 * of its own would have cost a multi-KB stack against a free-heap floor of
 * 9216 B that M4 held at 10964 B.
 */
#include <stdint.h>
#include <stdbool.h>

/* Injected scan-restart hook -- see gatt_engine_set_scan_resume(). Returns
 * true iff scanning is actually running when it returns. */
typedef bool (*gatt_scan_resume_fn_t)(void);

/* Injected "somebody else has the radio" predicate -- see
 * gatt_engine_set_conn_busy_hook(). */
typedef bool (*gatt_conn_busy_fn_t)(void);

/* Creates the per-attempt deadline timer and the ble_npl events used to
 * hop onto the NimBLE host task. MUST be called after nimble_port_init()
 * (the npl function table this uses does not exist before it). Until it has
 * run, gatt_engine_request() is a no-op. */
void gatt_engine_init(void);

/* Installs the "start scanning again" callback. Separate from
 * gatt_engine_init() for exactly the reason wrapper_arena_set_loader() is
 * separate from wrapper_arena_init(): restarting the scan means re-issuing
 * ble_gap_disc() with ble_collector.c's own scan parameters, and this
 * component must not depend on ble_collector (which already depends on
 * this one). ble_collector_start() passes ble_collector_resume_scan().
 *
 * The hook MUST report whether scanning is actually running when it
 * returns, not merely that a call was made. ble_gap_disc() returns
 * BLE_HS_EBUSY whenever a connect procedure is still outstanding
 * (ble_gap_disc_ext_validate() -> ble_gap_conn_active(), verified in the
 * stack source), and nothing anywhere in this firmware supervises scan
 * health -- so a single unnoticed EBUSY means the hub is deaf to every
 * advertisement for the rest of the boot, with the drop counter in
 * /api/v1/status not even moving, because nothing is being received to
 * drop. This engine retries on a false return; see gatt_engine.c.
 *
 * A NULL hook is logged as an error, not quietly tolerated. */
void gatt_engine_set_scan_resume(gatt_scan_resume_fn_t fn);

/* Installs the predicate that reports whether the hub's OTHER outbound-
 * connection owner (battery_poll.c's MiFlora battery poll) currently holds
 * the radio. CONFIG_BT_NIMBLE_MAX_CONNECTIONS is 1 and the two schedulers
 * are independent, so without this a poll and a GATT read can collide: the
 * loser's ble_gap_connect() fails and this engine would record a failure, a
 * backoff and a handle-cache drop against a device that did nothing wrong.
 * Checked immediately before connecting. Optional: a NULL hook means "no
 * other owner exists", which is the correct answer on a build where battery
 * polling is not running. */
void gatt_engine_set_conn_busy_hook(gatt_conn_busy_fn_t fn);

/* True while an attempt, or the tail of one, is in flight. The trigger in
 * ble_collector.c checks this before requesting, and gatt_engine_request()
 * checks it again: spec section 7 requires a request arriving while the
 * manager is busy to be DROPPED, never queued. Dropping is correct because
 * the trigger is an advertisement: the device will advertise again, and a
 * queued request would connect to a device whose connectable window has
 * already closed.
 *
 * It stays true until a decode still owed has run (the read buffer belongs
 * to the finished attempt and a new one would overwrite it) and until a
 * scan restart that failed has stopped being retried (a new attempt would
 * stop scanning again mid-retry and burn the retry budget). */
bool gatt_engine_busy(void);

/* Requests a GATT read of dev_idx (a data_core registry slot index).
 * Called from adv_decoder_task ONLY -- it reads the wrapper's connect plan
 * out of the wrapper arena, which is flash-backed.
 *
 * mac/addr_type are the RAW GAP address, exactly as ble_collector.c's
 * adv_item_t carries it (ble_addr_t.val[] on-air order, val[0] is the LAST
 * byte a human reads) -- NOT the display order device_id_from_mac() and
 * wrapper_exec_run() take. This file reverses it once, internally, for the
 * decode side; see gatt_engine.c. Passing display order here would connect
 * to a nonexistent address.
 *
 * Sets a request and returns immediately: no radio call happens on the
 * caller's task. Silently does nothing when the engine is busy, when
 * dev_idx is out of range, or when the wrapper turns out to carry no plan. */
void gatt_engine_request(uint16_t wrapper_id, int dev_idx, const uint8_t mac[6],
                         uint8_t addr_type);

/* Runs the decoder-task half of an attempt: the wrapper decode a GA_DECODE
 * asked for. Call it from adv_decoder_task's loop, at the same safe point
 * the wrapper reindex is performed at (never mid-decode of an
 * advertisement). Cheap and a no-op when there is nothing owed. */
void gatt_engine_service(void);

/* The last failure reason recorded for dev_idx, "" when there is none
 * (never NULL -- spec section 5 puts this on the device's API surface and
 * Task 7 renders it). Returned pointers are string literals with static
 * lifetime, so the caller may hold one indefinitely.
 *
 * Set on every failed attempt and cleared on a successful one -- with one
 * deliberate exception: an attempt whose reads all succeeded but whose
 * wrapper decode emitted nothing records that instead of clearing. Since
 * the amended spec section 5 cut M5a's event-log entry, this string and
 * gatt_sched.h's fail count and last-successful-read timestamp are the
 * ENTIRE visibility surface for a connect block that contributes nothing --
 * which is the failure section 5 exists to make loud. */
const char *gatt_engine_last_error(int dev_idx);
