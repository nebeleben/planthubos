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
 * places where it genuinely had to (the ATT-handle/uuid16 mapping -- now a
 * server-side read-by-UUID resolution on every connection, not a cache,
 * per the M5a hardware gate's own finding -- what counts as a
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

/* How a command attempt ended (M5b Task 8) -- see
 * gatt_engine_set_cmd_done_hook().
 *
 *   ok=true,  confirmed=true   the write landed AND the confirm read's
 *                              require was satisfied.
 *   ok=true,  confirmed=false  the write landed and the action declared no
 *                              confirm block: COMPLETED UNCONFIRMED, which
 *                              spec section 4.4 treats as its own outcome,
 *                              not as a success.
 *   ok=false                   the command failed; err names the reason
 *                              (never NULL on this path, always a string
 *                              literal, same discipline as
 *                              gatt_engine_last_error()). A confirm read
 *                              whose require was NOT satisfied lands here
 *                              -- the device answered and said no, which is
 *                              louder than unconfirmed.
 *
 * Runs on the NimBLE host task, inside the same call that ends the attempt
 * and restarts scanning. It must not block, must not touch flash, and must
 * not call back into this engine. */
typedef void (*gatt_cmd_done_fn_t)(int dev_idx, bool ok, bool confirmed, const char *err);

/* The one failure reason that means "nothing was wrong with this command,
 * the radio was simply not ours at that moment" -- the other owner of the
 * hub's single outbound connection (battery_poll.c) held it. Every other
 * `err` this engine reports describes something that will not fix itself:
 * a device that refused the write, an entry that will not parse, a
 * parameter over its bound.
 *
 * Exported so the hook can tell the two apart, and compared BY POINTER
 * rather than by strcmp(): it is the same string object the engine passed,
 * so identity is exact and no reader has to depend on the wording of a log
 * message. A command refused with THIS reason has already been popped from
 * the actor queue and charged against its hourly budget, so the hook is
 * expected to put it back on the queue rather than let it die -- see
 * ble_collector.c, which does exactly that, once, bounded by the command's
 * own TTL (fix round 1, Critical 2). */
extern const char *const GATT_CMD_ERR_RADIO_BUSY;

/* Largest one-entry action section gatt_engine_request_command() accepts,
 * so a caller can size its own buffer: psvm.h's PSVM_FLAG_ACTION_TABLE
 * layout is u8 action_count + action_id(1) + param_max(2) + flags(1) +
 * write_uuid16(2) + write_len(1) + up to PSVM_PLAN_WRITE_MAX (8) write
 * bytes + param_offset/param_encoding(2) + an optional 8-byte confirm
 * block. Spelled out rather than derived from psvm.h here for the same
 * reason gatt_fsm.h duplicates the plan limits (this header must be
 * includable without a psvm dependency); gatt_engine.c carries the
 * _Static_assert that keeps the two in step. */
#define GATT_ACTION_ENTRY_MAX 26

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
 * loser's ble_gap_connect() fails and this engine would record a failure
 * and a backoff against a device that did nothing wrong.
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

/* Installs the completion callback for gatt_engine_request_command().
 *
 * A hook, not a direct call into components/actors, and deliberately: the
 * actor layer calls INTO this engine and this engine must report back, so a
 * REQUIRES in both directions would be an ESP-IDF component cycle and a
 * hard build failure. Same shape, same reason, as the two hooks above --
 * the component that owns the composition (ble_collector_start()) registers
 * both halves. A NULL hook is a safe no-op, but it means a command's
 * outcome goes nowhere, so the wiring is not optional in a real build.
 *
 * The engine takes plain scalars plus the raw action-entry bytes for the
 * same reason: an actor_cmd_t across this boundary would need the header
 * that defines it. */
void gatt_engine_set_cmd_done_hook(gatt_cmd_done_fn_t fn);

/* M5b Task 8: executes ONE actuator command against dev_idx -- connect,
 * write, optionally read back and evaluate `require`, disconnect. Called
 * from adv_decoder_task ONLY (like gatt_engine_request(): resolving the
 * action entry reads the wrapper arena, which is flash-backed).
 *
 * (action_entry, entry_len) is the one-entry action section
 * gatt_fsm_init_command() parses -- wrapper_exec_action_get() produces
 * exactly it. The bytes are COPIED here, so the caller's buffer need not
 * outlive the call (and must not be an arena pointer held across the
 * attempt). mac/addr_type are the RAW GAP address, exactly as
 * gatt_engine_request() takes them.
 *
 * A COMMAND TAKES PRIORITY OVER A SCHEDULED READ (spec section 3): a read
 * deferred by one interval is invisible, a command deferred is a valve that
 * did not open. So, unlike gatt_engine_request(), this is NOT dropped when
 * the engine is busy -- it is held and started the moment the radio frees
 * up, and while it is held gatt_engine_busy() reports true so no new read
 * can start ahead of it. It is bounded, not indefinite: the attempt it is
 * waiting behind has M5a's 5-second deadline.
 *
 * Every command that cannot be executed reports through the done hook
 * instead of vanishing -- including one refused before it ever starts (a
 * second command still pending, the other radio owner holding the
 * connection, an entry that does not parse). A dispatched command has
 * already been popped from the actor queue and charged against its hourly
 * budget, so a silent drop here would be an actuator that never moved and
 * nothing anywhere saying so.
 *
 * A confirm read's decoded value is stored into the device's
 * CAP_SWITCH_STATE slot (spec section 4.4) -- see gatt_engine_service(),
 * which is where that write actually happens. */
void gatt_engine_request_command(int dev_idx, const uint8_t *action_entry, uint16_t entry_len,
                                 uint16_t param, const uint8_t mac[6], uint8_t addr_type);

/* True while a command is waiting for the radio or executing. The caller
 * that pumps the actor queue (ble_collector.c's decoder loop) checks this
 * before dispatching the next command: this engine holds exactly ONE
 * command, and a second dispatched on top of it would have to be refused --
 * whereas leaving it in the actor queue keeps its TTL and the safety
 * priority rule intact until the radio can actually take it. */
bool gatt_engine_cmd_busy(void);

/* Runs the decoder-task half of an attempt: the wrapper decode a GA_DECODE
 * asked for. Call it from adv_decoder_task's loop, at the same safe point
 * the wrapper reindex is performed at (never mid-decode of an
 * advertisement). Cheap and a no-op when there is nothing owed.
 *
 * It also performs the OTHER piece of work an attempt can owe this task: a
 * command's confirm read updates the device's CAP_SWITCH_STATE, and that
 * write goes through data_core_submit_cap(), which takes the registry mutex
 * and posts DATA_EVENT_SENSOR_UPDATE (whose subscribers are sized for this
 * task's stack, not the NimBLE host task's). battery_poll.c hops its own
 * registry write off the host task for exactly this reason. */
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
