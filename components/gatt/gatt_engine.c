/* gatt_engine.c -- see gatt_engine.h for the contract and, above all, for
 * the task-ownership rules this file exists to honour.
 *
 * The shape of this file is deliberately flat: one translator (perform())
 * that turns a gatt_act_t into a NimBLE call, one feeder (engine_feed())
 * that hands NimBLE's answer back to the state machine, and one place an
 * attempt can end (attempt_finish()). Every callback below does the same
 * three things -- classify what NimBLE just said, hand it to
 * gatt_fsm_step() as an event, and let the returned action decide what
 * happens next. Nothing here inspects gatt_fsm_t.state to decide what to do
 * next, with a single documented exception (attempt_finish() reads it to
 * tell a successful attempt from a failed one, which is the one fact the
 * state machine can only express as "which terminal state did we reach").
 */
#include "gatt_engine.h"
#include "gatt_fsm.h"
#include "gatt_sched.h"
#include "wrapper_exec.h"
#include "psvm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include <string.h>

static const char *TAG = "gatt_engine";

/* Design spec section 3's hard deadline. PER ATTEMPT, not per operation:
 * one timer is armed when the attempt starts and disarmed when it ends by
 * any route. A per-operation timer would let a device that answers every
 * single step slowly hold the radio -- and with it the hub's entire
 * advertisement intake, since scanning is stopped while connected --
 * indefinitely without ever technically timing out. */
#define GATT_ATTEMPT_DEADLINE_MS 5000

/* Deliberately SHORTER than the attempt deadline (fix round 1, Critical 1,
 * contributor 1). Handing ble_gap_connect() the same 5000 ms meant our
 * microsecond-resolution timer always beat NimBLE's later, tick-granular
 * one, so the single most common real failure -- a battery sensor that went
 * back to sleep before the connect landed -- always took the timeout path
 * instead of the CONNECT-with-nonzero-status path that already works and
 * that leaves the stack's master state cleanly reset. With this margin the
 * ordinary case is handled by the ordinary path and our timer is what it
 * was meant to be: a backstop.
 *
 * This value ALSO bounds something that happens entirely outside our
 * attempt, which is why the scan-retry budget below is sized against it
 * (fix round 2, N3/N4 -- an earlier version of this comment claimed the
 * stack's reattempts were "still bounded by" our window, which is simply
 * false). CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT is on with
 * CONFIG_BT_NIMBLE_MAX_CONN_REATTEMPT = 3: when a master link breaks with
 * BLE_ERR_CONN_ESTABLISHMENT, ble_hs_hci_evt.c aborts our GATT procedure,
 * DELETES the connection (so no BLE_GAP_EVENT_DISCONNECT ever reaches us --
 * we see only the aborted read/write callback) and re-issues
 * ble_gap_connect() with a FRESH copy of this duration, not the remainder.
 * That reattempt is issued after our attempt has already ended, so it holds
 * the radio -- and blocks ble_gap_disc() with BLE_HS_EBUSY -- for up to
 * another full GATT_CONNECT_TIMEOUT_MS that nothing in this file started or
 * can shorten. Its outcome does come back to gatt_gap_event() as a CONNECT
 * event with s_active already false, which is why that branch restarts
 * scanning too. */
#define GATT_CONNECT_TIMEOUT_MS 3000

/* Not a second deadline: this is armed only AFTER the attempt deadline has
 * already expired and a disconnect has been commanded, to bound the wait
 * for the event that confirms it. Without it, a teardown whose confirmation
 * never arrives would leave s_active true forever, and since restarting the
 * scan is part of ending an attempt, the hub would stay deaf to every
 * advertisement for the rest of the boot -- the exact failure the deadline
 * exists to prevent, reintroduced one step later. battery_poll.c's
 * POLL_WATCHDOG_S guards the same hazard. */
#define GATT_TEARDOWN_GRACE_MS 1000

/* Scan-restart retry (fix round 1, Critical 1, contributor 3). ble_gap_disc()
 * returns BLE_HS_EBUSY while a connect procedure is still outstanding, and
 * ble_gap_conn_cancel() does NOT clear that state synchronously -- it only
 * transmits the HCI cancel and sets master.conn.cancel, with master.op
 * cleared later, when the controller reports the aborted connection (read
 * out of ble_gap.c, not assumed). So a restart can legitimately fail for a
 * while after an attempt ends.
 *
 * The budget is sized against the LONGEST such block, not the shortest
 * (fix round 2, N3): a cancel completing is milliseconds, but a stack-issued
 * connection reattempt holds the radio for up to a fresh
 * GATT_CONNECT_TIMEOUT_MS (3000 ms -- see its comment above). The round-1
 * budget of 10 x 250 ms = 2500 ms was provably shorter than exactly the
 * case that most needs it, exhausting ~500 ms early and leaving the hub
 * deaf. 32 x 250 ms = 8000 ms clears one reattempt window by 2.7x, and
 * still covers it even after the one extra retry a stale timer post can
 * consume (see on_deadline()). 250 ms is kept as the step so an ordinary
 * few-millisecond block still clears almost immediately; in the common case
 * the very first attempt succeeds and no timer is armed at all. */
#define GATT_SCAN_RETRY_MS  250
#define GATT_SCAN_RETRY_MAX 32
_Static_assert(GATT_SCAN_RETRY_MS * GATT_SCAN_RETRY_MAX > GATT_CONNECT_TIMEOUT_MS,
               "the scan-restart budget must outlast a stack-issued connection reattempt, "
               "which holds the radio for a fresh GATT_CONNECT_TIMEOUT_MS after our attempt ends");

/* Largest connect-plan section psvm.h's on-blob layout can produce:
 * u8 read_count + u8 write_count + u32 interval_s, then the read uuid16s,
 * then the writes (u16 uuid16 + u8 len + up to PSVM_PLAN_WRITE_MAX bytes
 * each). Sized from psvm.h's own constants so it cannot drift from the
 * format. The plan is COPIED here rather than pointed at inside the wrapper
 * arena: gatt_fsm_t.write[].data points into whatever buffer
 * gatt_fsm_init() was given and is dereferenced later, on another task,
 * while an arena eviction from any caller can invalidate an arena pointer
 * at any time (wrapper_arena.h's FINDING 2 doc comment). */
#define GATT_PLAN_MAX (6 + PSVM_PLAN_MAX_READS * 2 + \
                       PSVM_PLAN_MAX_WRITES * (3 + PSVM_PLAN_WRITE_MAX))

/* What the single esp_timer is currently counting down, so its one callback
 * can tell three unrelated deadlines apart. They can never overlap: the
 * teardown grace only starts once the attempt deadline has expired, and a
 * scan retry only starts once the attempt has ended (and gatt_engine_busy()
 * reports busy while one is pending, so no new attempt can begin under it).
 *
 * The phase is read by on_deadline() on the host task, but the post that
 * gets it there carries no phase of its own, so a post can outlive the
 * phase it was made in. Two things bound that, and it is worth being exact
 * about which one does the work, because an earlier version of this comment
 * credited the wrong one (fix round 3).
 *
 * First: all three phases share the one s_ev_deadline event, and
 * ble_npl_eventq_put() no-ops on an event that is already queued, so at most
 * ONE deadline post is ever in flight. A second ble_npl_event for any of
 * these phases would remove that and need a real generation counter in its
 * place.
 *
 * Second, and this is the part that actually makes a stale post harmless:
 * the timer is never left armed by a function that is not about to service
 * it. resume_scan_or_retry() stops it on entry and therefore owns it on
 * every path out, and the attempt/teardown arms stop first too. Re-checking
 * s_active in each handler branch is NOT sufficient on its own and must not
 * be read as if it were: once the phase can move TP_SCAN_RETRY -> TP_IDLE ->
 * TP_ATTEMPT with a post in flight, a stale post arriving in TP_ATTEMPT sees
 * a perfectly live s_active and is indistinguishable from a real deadline --
 * it would time out a brand-new attempt at t~0 and record a failure and a
 * backoff against an innocent device. Nothing about the shared event stops
 * that; only never leaving a stray one-shot armed does. */
typedef enum { TP_IDLE, TP_ATTEMPT, TP_TEARDOWN, TP_SCAN_RETRY } timer_phase_t;

/* ---------------- state ----------------
 *
 * All of this is static, per spec section 6's budget: one attempt at a time
 * is the same single-writer argument the shared read buffer rests on.
 *
 * The NimBLE host task is the only WRITER of everything here. Two fields
 * are also read from adv_decoder_task and are marked as such; see
 * gatt_engine.h's task-ownership note for why that is safe and what would
 * not be.
 */
static gatt_fsm_t s_fsm;
static bool     s_active;          /* attempt open: set by on_start_req(), cleared by attempt_finish().
                                    * Also READ from adv_decoder_task via gatt_engine_busy(). */
/* Did THIS attempt's ble_gap_connect() actually start a connect procedure?
 * ble_gap_conn_cancel() is process-global -- it cancels whatever connect is
 * outstanding, with no notion of whose it is -- so cancelling without this
 * would abort battery_poll.c's poll instead of ours. See the GA_DISCONNECT
 * case in perform() for the sequence that made that reachable. */
static bool     s_own_conn_proc;
static timer_phase_t s_phase;
static uint8_t  s_scan_retries;
static uint16_t s_wrapper_id;
static int      s_dev_idx = -1;
static uint8_t  s_mac_gap[6];      /* raw GAP/on-air order -- what ble_gap_connect() wants */
static uint8_t  s_mac_disp[6];     /* display/human order -- what wrapper_exec_run_buffer() wants */
static uint8_t  s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_pending_uuid16;  /* the characteristic the in-flight read/write is for */
/* True once the in-flight read-by-UUID (a GA_READ, or the handle-resolve
 * half of a GA_WRITE -- see write_resolve_cb()) has delivered its one
 * matching attribute. NimBLE's read-by-UUID procedure fires its callback
 * once per matching attribute (status 0) and once more with status
 * BLE_HS_EDONE when the procedure completes; this guards against a second
 * status-0 hit (a peer exposing more than one instance of the same
 * characteristic, which M5a does not support) being mistaken for the
 * genuine completion, exactly like battery_poll.c's own s_got_result. Reset
 * at the start of every GA_READ/GA_WRITE in perform(). */
static bool     s_uuid_op_got_result;
/* Where a read-by-UUID's matching attribute is PARKED until its procedure
 * terminates. The engine must not advance the state machine from the
 * status-0 callback: gatt_fsm_step() runs synchronously inside
 * engine_feed(), so the very next plan action -- another read -- would be
 * issued from inside the previous procedure, resetting s_pending_uuid16 and
 * s_uuid_op_got_result while that procedure is still live. Its terminating
 * BLE_HS_EDONE would then arrive against the NEXT read's state, find
 * s_uuid_op_got_result false, and report the next characteristic as "not
 * found" -- which is precisely what the M5a hardware gate observed: a
 * temperature read that the peer served correctly, followed instantly by
 * "read of 0x2A6F: characteristic not found" and a teardown, while the peer
 * went on to serve the humidity read into a connection that was already
 * closing. The plain ble_gattc_read() this replaced had a single callback
 * and no terminator, so the old cached-handle path never had a second
 * callback to misattribute. Parking here and feeding the machine from the
 * EDONE branch keeps it to exactly one event per procedure, raised only
 * after NimBLE has finished with that procedure. */
static uint8_t  s_read_buf[GATT_FSM_SLOT];
static uint8_t  s_read_len;
static bool     s_read_copy_failed;
static uint16_t s_resolved_handle;   /* GA_WRITE's resolved target, parked for the same reason */
static const uint8_t *s_pending_write_data;  /* GA_WRITE's payload -- points into s_plan */
static uint8_t  s_pending_write_len;
static uint8_t  s_plan[GATT_PLAN_MAX];
static uint16_t s_plan_len;

/* Why the failure reason is a const char * and not a formatted buffer:
 * spec section 6 budgets ~200 B for the whole connection manager, and 16
 * devices x even a modest fixed-size string is several times that. Every
 * value ever stored here is a string literal chosen at the point of
 * failure, so a per-device pointer table costs 64 B and keeps the budget
 * honest, and a reader on another task can never see a half-written string
 * -- only one pointer or the other, both to immortal storage. The NimBLE
 * status code that a formatted string would have carried goes to the log
 * line at the point of failure, where it is actually useful for diagnosis,
 * rather than into a UI field whose job is to say WHAT failed in words a
 * user can act on. */
static const char *s_last_error[GATT_SCHED_MAX_DEVICES];
static const char *s_err;          /* reason for the failure currently being processed */

/* The two cross-task flags, each written by one task and cleared by the
 * other -- plain volatile bools, exactly like ble_collector.c's
 * s_wrapper_reindex_pending: one bit of information each, and nothing else
 * depends on their ordering relative to anything but the fields they gate. */
static volatile bool s_req_pending;      /* decoder task -> host task */
static volatile bool s_decode_pending;   /* host task -> decoder task */
static uint8_t  s_decode_len;
/* Written by BOTH tasks, at points that cannot overlap: the host task
 * clears it when an attempt starts, adv_decoder_task sets it to the decode's
 * result, and the host task reads it only after the GE_DECODED that the
 * decoder task posts AFTER writing it. Not a data race, but it is not a
 * single-writer field either, and claiming otherwise would be a false
 * invariant. */
static bool     s_decode_wrote;

static esp_timer_handle_t s_deadline;
static struct ble_npl_event s_ev_start;
static struct ble_npl_event s_ev_deadline;
static struct ble_npl_event s_ev_decoded;
static gatt_scan_resume_fn_t s_resume_scan;
static gatt_conn_busy_fn_t   s_conn_busy;
static bool s_inited;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void post_to_host(struct ble_npl_event *ev)
{
    /* ble_npl_eventq_put() is a no-op for an event already queued (the
     * FreeRTOS npl port checks ev->queued and clears it on dequeue, before
     * the callback runs), so a duplicate post can never double-run a
     * handler or grow the queue. Safe from any task. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), ev);
}

/* ---------------- getting scanning back ----------------
 *
 * Restarting the scan is the single most important thing this file does,
 * and it is the one radio operation that can fail for reasons that have
 * nothing to do with the attempt that just ended: ble_gap_disc() returns
 * BLE_HS_EBUSY while any connect procedure is still outstanding. Nothing in
 * this firmware supervises scan health, so an unnoticed failure here is
 * permanent deafness -- and invisible, because /api/v1/status's adv_dropped
 * counter cannot rise when nothing is being received to drop. So the
 * restart is verified and retried rather than merely attempted.
 *
 * If every retry fails, this logs at ERROR and stops. That is a deliberate
 * end state rather than an infinite retry: seconds of continuous EBUSY mean
 * something reissuing the same call cannot fix, and a timer looping forever
 * would hide it.
 *
 * What actually recovers the hub after that is worth being honest about
 * (fix round 2), because an earlier version of this comment listed three
 * paths and two of them are dead exactly when they would be needed: a hub
 * that is not scanning receives no advertisements, so no advertisement can
 * trigger the next GATT attempt, and BLE_GAP_EVENT_DISC_COMPLETE cannot
 * fire for a scan that never started. The one live path is battery_poll.c,
 * whose 60-second tick is timer-driven rather than advertisement-driven:
 * the next poll to come due terminates by calling
 * ble_collector_resume_scan() itself. That needs a known MiFlora and can be
 * up to a day away. On a hub with none, the recovery is a reboot -- which
 * is what the ERROR line is for. The stack-issued reattempt paths in
 * gatt_gap_event() are the other way back, and they are why those branches
 * call this function even with no attempt of ours open. */
static void resume_scan_or_retry(void)
{
    /* This function OWNS the deadline timer for as long as it runs, and
     * every path out of it leaves the timer in a state this function chose
     * (fix round 3). Stopping here rather than just before the re-arm is
     * the whole of that guarantee: the success path and the
     * budget-exhausted path below both set TP_IDLE and return, and before
     * this line they could return with a one-shot from an earlier retry
     * still armed.
     *
     * That was harmless while the only ways in were attempt_finish() (which
     * stops first) and on_deadline()'s already-fired one-shot. Round 2 added
     * three call sites in gatt_gap_event() whose entire purpose is to be
     * reached mid-retry, and a live one-shot surviving into TP_IDLE is not a
     * cosmetic leak: gatt_engine_busy() goes false, a new request is
     * accepted, and if the host task is behind by up to GATT_SCAN_RETRY_MS
     * the queue order becomes on_start_req (arming TP_ATTEMPT, its own stop
     * a no-op on an already-fired timer) followed by the stale post, which
     * takes the TP_ATTEMPT branch with s_active true and times out a
     * brand-new attempt at t~0 -- a "timed out", a gatt_sched_fail() and a
     * backoff against a device that did nothing wrong. Self-healing, and
     * still exactly the harm the round-2 fixes existed to stop. */
    esp_timer_stop(s_deadline);

    if (!s_resume_scan) {
        ESP_LOGE(TAG, "no scan-resume hook installed: scanning is NOT running and this hub "
                      "is now deaf to advertisements");
        s_phase = TP_IDLE;
        return;
    }
    if (s_resume_scan()) {
        s_scan_retries = 0;
        s_phase = TP_IDLE;
        return;
    }
    if (s_scan_retries >= GATT_SCAN_RETRY_MAX) {
        ESP_LOGE(TAG, "scanning could not be restarted after %d retries over %d ms -- this hub "
                      "is deaf to advertisements until something else restarts the scan",
                 GATT_SCAN_RETRY_MAX, GATT_SCAN_RETRY_MAX * GATT_SCAN_RETRY_MS);
        s_scan_retries = 0;
        s_phase = TP_IDLE;
        return;
    }
    s_scan_retries++;
    s_phase = TP_SCAN_RETRY;
    /* The timer is guaranteed stopped by the top of this function, so this
     * arm cannot fail with ESP_ERR_INVALID_STATE the way it did before fix
     * round 2 -- when re-entry while the retry timer was still armed made
     * the arm fail, and that failure was then handled as "the timer is
     * broken": budget reset, phase dropped to TP_IDLE, a log line blaming
     * the timer, and the loop abandoned after two of its retries. Re-entry
     * now simply restarts the interval, costing one retry from a budget
     * GATT_SCAN_RETRY_MAX is sized to absorb. */
    if (esp_timer_start_once(s_deadline, (uint64_t)GATT_SCAN_RETRY_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "scan-retry timer failed to arm; scanning is NOT running");
        s_scan_retries = 0;
        s_phase = TP_IDLE;
    }
}

/* ---------------- the one place an attempt ends ----------------
 *
 * SUCCESS, read error, timeout, an unsolicited disconnect, a connect that
 * never established, a NimBLE call refused at issue time -- every route out
 * of an attempt passes through here, and this function ALWAYS goes on to
 * restart scanning. That is the whole reason it exists as a single function
 * rather than as a line in each callback: a hub that stops scanning after a
 * failed connection is worse than one that never connected, because it goes
 * silently deaf to every advertisement afterwards -- including the ones
 * that would have triggered a retry.
 *
 * (The brief's wording was "put the restart in the disconnect callback so
 * it cannot be forgotten per-path". This is that rule, widened: a connect
 * that never establishes produces a BLE_GAP_EVENT_CONNECT with a nonzero
 * status and never a disconnect callback at all, so the disconnect callback
 * alone would have missed exactly the path that most needs the scan back.
 * The disconnect callback routes here like everything else.) */
static void attempt_finish(void)
{
    if (!s_active) return;
    esp_timer_stop(s_deadline);
    s_phase = TP_IDLE;

    /* The one fact only the state machine knows: which terminal state this
     * attempt reached. GS_DONE is reached solely via GE_DECODED, i.e. every
     * declared read landed and the decode ran. */
    bool radio_ok = (s_fsm.state == GS_DONE);
    uint32_t now = now_s();

    if (radio_ok && s_decode_wrote) {
        gatt_sched_ok(s_dev_idx, now);
        ESP_LOGD(TAG, "gatt read ok: dev=%d", s_dev_idx);
    } else if (radio_ok) {
        /* Third outcome (fix round 1): the radio behaved perfectly and the
         * wrapper emitted nothing. Not a failure -- no backoff, the reads
         * are demonstrably fine -- but NOT a successful read either:
         * gatt_sched_attempt() moves the interval gate without advancing
         * the last-successful-read timestamp section 8 renders, so a
         * connect block contributing nothing stays visibly silent instead
         * of looking freshly read. See gatt_sched.h. */
        gatt_sched_attempt(s_dev_idx, now);
        ESP_LOGI(TAG, "gatt read ok but the wrapper emitted nothing: dev=%d wrapper=%u",
                 s_dev_idx, (unsigned)s_wrapper_id);
    } else {
        gatt_sched_fail(s_dev_idx, now);
        /* No handle cache to drop any more (removed during the M5a
         * hardware gate: every read and write resolves its uuid16 fresh,
         * server-side, on every connection -- see GA_READ/GA_WRITE in
         * perform()), so a failed attempt here needs no extra cleanup
         * beyond the backoff bookkeeping gatt_sched_fail() already did. */
        ESP_LOGW(TAG, "gatt read failed: dev=%d %s (consecutive failures: %u)",
                 s_dev_idx, s_err ? s_err : "unknown error",
                 (unsigned)gatt_sched_fail_count(s_dev_idx));
    }

    if (s_dev_idx >= 0 && s_dev_idx < GATT_SCHED_MAX_DEVICES) {
        if (!radio_ok) {
            s_last_error[s_dev_idx] = s_err;
        } else if (!s_decode_wrote) {
            s_last_error[s_dev_idx] = "read ok, decode emitted nothing";
        } else {
            s_last_error[s_dev_idx] = NULL;
        }
    }

    /* No event-log entry. M5a's spec section 5 originally required one per
     * attempt; the amended section 5 CUT it, because event_log_append()
     * writes a LittleFS record and then runs the SSE hook, whose
     * main.c on_event_logged() opens with a ~2 KB rule_info_t array in one
     * frame -- a chain rules_engine.c:22-27 already sized ITS task at
     * 8192 B for. The only tasks that may write it are ones with a stack
     * to match, and neither raising adv_decoder_task's 3072 B nor adding a
     * task fits section 6's 9216 B free-heap floor. Visibility is met by
     * the surface a user actually looks at instead: last_error above,
     * gatt_sched's fail count, and the last successful read, all rendered
     * by Task 7 in the Devices tab. Do not restore an event_log_append()
     * here without re-reading that amendment -- the stack cannot hold it. */

    s_active = false;
    s_own_conn_proc = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    resume_scan_or_retry();
}

/* Defined below; declared here because perform() hands each of them to
 * NimBLE as the callback for the operation it issues. */
static int gatt_gap_event(struct ble_gap_event *event, void *arg);
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg);
static int write_resolve_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg);
static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg);

/* ---------------- the translator ----------------
 *
 * One gatt_act_t in, one radio operation out. This function is the entire
 * reason gatt_fsm.c can be pure: everything below is a mechanical rendering
 * of an action the state machine already chose, and nothing here chooses
 * what happens next. Returns true when the operation could not even be
 * ISSUED (NimBLE refused it synchronously, so no callback will ever arrive
 * for it) and the caller must therefore feed the state machine a GE_ERROR
 * itself; see engine_feed(). */
static bool perform(const gatt_act_t *a)
{
    switch (a->kind) {
    case GA_NONE:
        return false;

    case GA_CONNECT: {
        /* Scanning must stop BEFORE connecting: a connect attempt issued
         * while a scan is running is a documented source of failures on
         * this stack. battery_poll.c's handle_tick() does the same thing
         * for the same reason, and tolerates BLE_HS_EALREADY the same way
         * (no scan running is not an error here). */
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGD(TAG, "disc_cancel before connect: %d (continuing)", rc);
        }
        ble_addr_t peer = { .type = s_addr_type };
        memcpy(peer.val, s_mac_gap, 6);
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, GATT_CONNECT_TIMEOUT_MS,
                             NULL, gatt_gap_event, NULL);
        if (rc != 0) {
            /* BLE_HS_EALREADY specifically means a connect procedure is
             * already running and it is NOT ours (ble_gap_connect() checks
             * ble_gap_conn_active() first) -- the other owner of the hub's
             * single outbound connection won the race that
             * gatt_engine_set_conn_busy_hook()'s check narrows but cannot
             * close. s_own_conn_proc stays false, which is what stops the
             * teardown below from cancelling their procedure. */
            ESP_LOGI(TAG, "ble_gap_connect refused: %d", rc);
            s_err = "connect refused";
            return true;
        }
        s_own_conn_proc = true;
        return false;
    }

    case GA_READ: {
        /* The state machine names characteristics by uuid16 and never sees
         * a handle (spec section 4, as amended during the M5a hardware
         * gate: handles are never written into a wrapper NOR cached across
         * a connection, and gatt_fsm.c holds itself to the same
         * discipline). Resolved here by NimBLE's read-by-UUID procedure --
         * an ATT Read By Type Request over the connection's whole handle
         * range -- which the SERVER resolves against its own attribute
         * table, so a shifted attribute table on the peer cannot mislead
         * it the way a stale cached handle could (this is exactly the
         * defect the hardware gate found: a cached handle that drifted
         * onto a DIFFERENT attribute -- a declaration, always readable --
         * "succeeded" with the wrong bytes). battery_poll.c's read_cb
         * establishes this same shape for MiFlora's single battery
         * characteristic; read_cb below follows it. */
        s_pending_uuid16 = a->uuid16;
        s_uuid_op_got_result = false;
        s_read_len = 0;
        s_read_copy_failed = false;
        int rc = ble_gattc_read_by_uuid(s_conn_handle, 1, 0xffff,
                                        BLE_UUID16_DECLARE(a->uuid16), read_cb, NULL);
        if (rc != 0) {
            ESP_LOGI(TAG, "read of 0x%04X refused: %d", (unsigned)a->uuid16, rc);
            s_err = "read refused";
            return true;
        }
        return false;
    }

    case GA_WRITE: {
        /* No handle to write to yet -- and, per GA_READ's comment above,
         * nothing here may cache one across a connection. So a write is
         * two ATT operations, not one: resolve the uuid16 to THIS
         * connection's handle via the same server-side read-by-UUID
         * procedure GA_READ uses (write_resolve_cb, below), then write to
         * the handle it hands back -- which is safe to use immediately
         * (the same connection, the same round trip's answer) even though
         * it is never stored anywhere afterward. The plan's optional
         * pre-read write is the ONLY write M5a performs; actuators and any
         * other state-changing write are M5b (spec section 9). a->data
         * points into s_plan, which outlives this call. */
        s_pending_uuid16 = a->uuid16;
        s_pending_write_data = a->data;
        s_pending_write_len = a->len;
        s_uuid_op_got_result = false;
        int rc = ble_gattc_read_by_uuid(s_conn_handle, 1, 0xffff,
                                        BLE_UUID16_DECLARE(a->uuid16), write_resolve_cb, NULL);
        if (rc != 0) {
            ESP_LOGI(TAG, "write resolve of 0x%04X refused: %d", (unsigned)a->uuid16, rc);
            s_err = "write refused";
            return true;
        }
        return false;
    }

    case GA_DECODE:
        /* Hand the concatenated read buffer to adv_decoder_task and stop.
         * The decode runs the wrapper VM and resolves its bytecode through
         * the flash-backed arena, neither of which may happen on the NimBLE
         * host task. gatt_engine_service() feeds GE_DECODED back when it is
         * done, which is what produces this attempt's single GA_DISCONNECT
         * -- deliberately NOT disconnecting here, so that every terminal
         * path including the success path has exactly one teardown rule
         * (gatt_fsm.h's invariant, pinned by a host test). */
        s_decode_len = a->len;
        s_decode_pending = true;
        return false;

    case GA_DISCONNECT: {
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) {
                /* The link is already gone; no disconnect event is coming. */
                attempt_finish();
            }
            return false;
        }

        /* No link. If we never started a connect procedure -- the connect
         * was refused, most importantly with BLE_HS_EALREADY because the
         * battery poller already had one running -- then there is nothing
         * of OURS outstanding and nothing to wait for, so finish now.
         *
         * Cancelling here instead would be actively destructive (fix round
         * 2, N1): ble_gap_conn_cancel() is process-global, so it would abort
         * the POLLER's connect, return 0, and leave this attempt open
         * waiting for a CONNECT event that gets delivered to the poller's
         * callback and never to ours -- hanging until the deadline plus the
         * grace with gatt_engine_busy() true throughout, destroying one
         * battery poll, and recording a failure and a backoff against a
         * device that did nothing wrong. That is the exact outcome the
         * mutual-awareness hook exists to prevent, and it was reachable
         * only through this path. */
        if (!s_own_conn_proc) {
            attempt_finish();
            return false;
        }

        /* Our own connect never established, and the PROCEDURE may still be
         * outstanding in the stack (fix round 1, Critical 1, contributor 2).
         * It must be cancelled explicitly: ble_gap_disc() refuses with
         * BLE_HS_EBUSY for as long as ble_gap_conn_active() is true, so
         * finishing here without cancelling would restart the scan into a
         * guaranteed EBUSY.
         *
         * And cancelling is not enough on its own: ble_gap_conn_cancel()
         * only transmits the HCI cancel and sets master.conn.cancel -- it
         * does NOT clear master.op, which the stack clears later, when the
         * controller reports the aborted connection (verified in
         * ble_gap.c, not assumed). So on a successful cancel the attempt is
         * deliberately left OPEN and ended by the resulting
         * BLE_GAP_EVENT_CONNECT instead, by which time the scan can
         * actually start -- ble_gap_master_connect_cancelled() resets master
         * state BEFORE invoking the callback. The teardown grace timer
         * bounds that wait, and the scan-restart retry covers it even if the
         * ordering still surprises us -- three independent guards, because
         * betting on ordering is betting on a race. */
        int rc = ble_gap_conn_cancel();
        if (rc == 0) return false;   /* cancel in flight: finish when it lands */
        if (rc != BLE_HS_EALREADY) {
            ESP_LOGI(TAG, "conn_cancel refused: %d (finishing anyway)", rc);
        }
        attempt_finish();
        return false;
    }

    case GA_REPORT_FAIL:
        /* The peer or the radio dropped the link for a reason nothing here
         * caused: gatt_fsm.h deliberately asks for a report rather than a
         * disconnect, because the link is already down and there is nothing
         * left to command. */
        attempt_finish();
        return false;

    default:
        return false;
    }
}

/* Feeds one event to the state machine and performs whatever it returns.
 * The loop exists only for the synchronous-refusal case: a NimBLE call that
 * fails at issue time produces no callback, so its GE_ERROR has to come
 * from here. The bound is a guard, not a design parameter -- the longest
 * real chain is two passes (the refused call, then the GA_DISCONNECT its
 * GE_ERROR produces). */
static void engine_feed(gatt_ev_kind_t kind, uint16_t handle,
                        const uint8_t *data, uint8_t len)
{
    gatt_ev_t ev = { .kind = kind, .handle = handle, .data = data, .len = len };
    for (int guard = 0; guard < 4; guard++) {
        gatt_act_t a = gatt_fsm_step(&s_fsm, &ev);
        if (!perform(&a)) return;
        ev.kind = GE_ERROR;
        ev.handle = 0;
        ev.data = NULL;
        ev.len = 0;
    }
    ESP_LOGW(TAG, "engine_feed guard tripped in state %d", (int)s_fsm.state);
}

/* Ends an attempt that has run out of time, closing whatever the stack may
 * still be holding first -- a live link, or an outstanding connect
 * procedure that would otherwise make the scan restart fail with EBUSY. */
static void force_close(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (s_own_conn_proc) {
        /* Same rule as perform()'s GA_DISCONNECT: only ever cancel a connect
         * procedure this attempt actually started, never whatever happens to
         * be outstanding process-wide. */
        ble_gap_conn_cancel();
    }
    attempt_finish();
}

/* ---------------- NimBLE callbacks (all on the host task) ---------------- */

static bool fsm_terminal(void)
{
    return s_fsm.state == GS_DONE || s_fsm.state == GS_FAILED;
}

static int gatt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* Either way the connect PROCEDURE is over: it either produced a
         * link or failed. Nothing left for us to cancel. */
        s_own_conn_proc = false;
        if (event->connect.status == 0) {
            if (!s_active || fsm_terminal()) {
                /* A connection that arrived for an attempt already given up
                 * on -- the classic case being a cancel that lost its race
                 * with the controller. Nothing owns this link, and with
                 * CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1 an unowned link would
                 * block every future attempt, so close it. If the attempt
                 * is still formally open, record the handle so the
                 * resulting disconnect is recognised as its ending. */
                ESP_LOGI(TAG, "connection arrived for an abandoned attempt; closing it");
                if (s_active) {
                    s_conn_handle = event->connect.conn_handle;
                    ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    return 0;
                }
                /* No attempt owns this at all -- typically a connection
                 * reattempt the STACK issued after our attempt already ended
                 * (see GATT_CONNECT_TIMEOUT_MS). Close it, and make sure
                 * scanning comes back: while that reattempt was running,
                 * ble_gap_disc() was refused with BLE_HS_EBUSY, so a
                 * scan-restart retry may have been burning its budget --
                 * or may already have given up. If the terminate lands, the
                 * disconnect below restarts scanning; if it does not, no
                 * disconnect is coming and this is the last chance. */
                if (ble_gap_terminate(event->connect.conn_handle,
                                       BLE_ERR_REM_USER_CONN_TERM) != 0) {
                    resume_scan_or_retry();
                }
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            engine_feed(GE_CONNECTED, 0, NULL, 0);
        } else {
            /* A connect failure, NimBLE's own connect timeout and the
             * completion of a ble_gap_conn_cancel() all land here with the
             * same nonzero status, and none of them leaves a link behind.
             * By the time this fires the stack's master state is clear, so
             * this is the path on which a scan restart actually succeeds --
             * which is why GATT_CONNECT_TIMEOUT_MS is set below the attempt
             * deadline, to make this the ordinary path rather than the
             * exception. */
            ESP_LOGI(TAG, "connect failed/timed out/cancelled: %d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (!s_active) {
                /* No attempt of ours is open, so this is the outcome of a
                 * connect the STACK re-issued on its own after ours ended
                 * (fix round 2, N3). That reattempt has been blocking
                 * ble_gap_disc() with BLE_HS_EBUSY for up to a full
                 * GATT_CONNECT_TIMEOUT_MS, and this event is the first
                 * moment a scan can succeed again -- so this path has to
                 * restart scanning too, or the hub stays deaf whenever the
                 * retry budget ran out first. Idempotent: if scanning is
                 * already running the hook reports success and this resets
                 * the retry state. */
                resume_scan_or_retry();
                return 0;
            }
            if (!fsm_terminal()) s_err = "connect failed";
            engine_feed(GE_ERROR, 0, NULL, 0);
            /* Terminal already (the attempt timed out and asked for a
             * cancel): gatt_fsm_step() ignored the event, so nothing above
             * ended the attempt. This is the moment it is really over. */
            if (s_active) attempt_finish();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (!s_active) {
            /* Nothing of ours is open, so this is a link that was closed
             * without an attempt behind it -- the abandoned-connection
             * branch above being the way that happens. Scanning was blocked
             * for as long as that link existed, so make sure it is back;
             * see the CONNECT branch's own note. */
            resume_scan_or_retry();
            return 0;
        }
        /* Ignore a disconnect belonging to a link this engine is not driving
         * while it IS driving another -- see stale_completion() for the same
         * argument applied to the GATT callbacks. Deliberately does NOT
         * restart scanning: an attempt is in flight and owns the radio. */
        if (event->disconnect.conn.conn_handle != s_conn_handle) return 0;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (!fsm_terminal()) {
            /* Unsolicited: the state machine turns this into GA_REPORT_FAIL,
             * which ends the attempt through attempt_finish() like everything
             * else. */
            s_err = "link dropped";
        }
        engine_feed(GE_DISCONNECTED, 0, NULL, 0);
        /* A disconnect we COMMANDED arrives in a terminal state, where the
         * state machine deliberately returns GA_NONE (gatt_fsm.h: DONE and
         * FAILED ignore every event, including the confirmation of the
         * disconnect they asked for). That confirmation is nonetheless the
         * moment the attempt is really over, so end it here. Guarded on
         * s_active so the GA_REPORT_FAIL path above, which already
         * finished, is not finished twice. */
        if (s_active) attempt_finish();
        return 0;

    default:
        return 0;
    }
}

/* A completion from a connection this engine is no longer driving: an
 * attempt that was forced closed while one of its GATT procedures was still
 * outstanding can still deliver callbacks (typically an aborted-procedure
 * error) after a NEW attempt has begun, where the error would fail an
 * attempt that is doing nothing wrong. NimBLE hands every GATT callback the
 * connection handle it belongs to, which is exactly the discriminator
 * needed -- so no generation counter of the kind battery_poll.c needs for
 * its queued messages is required here. */
static bool stale_completion(uint16_t conn_handle)
{
    return !s_active || conn_handle != s_conn_handle;
}

/* ble_gattc_read_by_uuid callback for GA_READ -- runs on the NimBLE host
 * task. Fires once per matching attribute (status 0) and once more with
 * status BLE_HS_EDONE when the procedure completes (attr NULL); M5a does
 * not support a peer exposing more than one instance of a plan's
 * characteristic, so s_uuid_op_got_result guards against a second status-0
 * hit being treated as a second completion (mirrors battery_poll.c's
 * read_cb, which establishes this exact shape for MiFlora's single battery
 * characteristic). No ATT-handle identity check is needed here the way the
 * old cached-handle path needed one: the SERVER matched this attribute
 * against the uuid16 we asked for, so what comes back is authoritatively
 * for that characteristic on THIS connection, whatever handle it happens to
 * live at -- which is exactly what makes this immune to the handle drift
 * that broke the cache (see gatt_fsm.h's gatt_fsm_init() doc comment). What
 * goes into the event is s_pending_uuid16 (the uuid16 we asked for), for
 * gatt_fsm.c's own identity check against the read it is currently
 * awaiting (gatt_ev_t.handle's doc comment) -- never an ATT handle, because
 * the state machine has never seen one. */
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (stale_completion(conn_handle)) return 0;

    if (error->status == 0 && attr != NULL) {
        if (s_uuid_op_got_result) return 0;   /* a second instance of this uuid16 on the peer: first wins */
        s_uuid_op_got_result = true;

        /* Park only -- the machine is fed from the EDONE branch below. See
         * s_read_buf's declaration for why feeding from here corrupts the
         * next read's state. */
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > GATT_FSM_SLOT) n = GATT_FSM_SLOT;   /* a longer value is truncated to its slot */
        s_read_copy_failed = (n > 0 && os_mbuf_copydata(attr->om, 0, n, s_read_buf) != 0);
        s_read_len = s_read_copy_failed ? 0 : (uint8_t)n;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!s_uuid_op_got_result) {
            ESP_LOGI(TAG, "read of 0x%04X: characteristic not found", (unsigned)s_pending_uuid16);
            s_err = "characteristic not found";
            engine_feed(GE_ERROR, 0, NULL, 0);
            return 0;
        }
        if (s_read_copy_failed) {
            s_err = "read failed";
            engine_feed(GE_ERROR, 0, NULL, 0);
            return 0;
        }
        /* A short read zero-pads its slot inside gatt_fsm_step(), which
         * re-zeroes the slot before copying -- no previous device's bytes
         * can survive in a slot this attempt did not fill. */
        engine_feed(GE_READ_OK, s_pending_uuid16, s_read_buf, s_read_len);
        return 0;
    }

    ESP_LOGI(TAG, "read of 0x%04X failed: %d", (unsigned)s_pending_uuid16, error->status);
    s_err = "read failed";
    engine_feed(GE_ERROR, 0, NULL, 0);
    return 0;
}

/* ble_gattc_read_by_uuid callback for GA_WRITE's handle-resolve half (see
 * perform()'s GA_WRITE case) -- the same procedure and the same
 * once-per-attribute/once-EDONE shape as read_cb above, but the resolved
 * attribute's handle is not this attempt's answer: it is what the actual
 * write (issued from here, via write_cb below) is addressed to. Nothing
 * about the resolved handle is stored anywhere past this function. */
static int write_resolve_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (stale_completion(conn_handle)) return 0;

    if (error->status == 0 && attr != NULL) {
        if (s_uuid_op_got_result) return 0;   /* a second instance of this uuid16 on the peer: first wins */
        s_uuid_op_got_result = true;

        /* Park the handle only. Issuing the write from here would start a
         * second GATT procedure inside the resolve procedure, and the
         * resolve's EDONE would then land on whatever state the write's own
         * completion had already moved on to -- the same misattribution
         * s_read_buf's declaration describes. */
        s_resolved_handle = attr->handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!s_uuid_op_got_result) {
            ESP_LOGI(TAG, "write of 0x%04X: characteristic not found", (unsigned)s_pending_uuid16);
            s_err = "characteristic not found";
            engine_feed(GE_ERROR, 0, NULL, 0);
            return 0;
        }
        int rc = ble_gattc_write_flat(conn_handle, s_resolved_handle, s_pending_write_data,
                                      s_pending_write_len, write_cb, NULL);
        if (rc != 0) {
            ESP_LOGI(TAG, "write of 0x%04X refused: %d", (unsigned)s_pending_uuid16, rc);
            s_err = "write refused";
            engine_feed(GE_ERROR, 0, NULL, 0);
        }
        /* else: the write is in flight and write_cb owns the event now. */
        return 0;
    }

    ESP_LOGI(TAG, "write resolve of 0x%04X failed: %d", (unsigned)s_pending_uuid16, error->status);
    s_err = "write refused";
    engine_feed(GE_ERROR, 0, NULL, 0);
    return 0;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (stale_completion(conn_handle)) return 0;

    if (error->status != 0) {
        ESP_LOGI(TAG, "write of 0x%04X failed: %d", (unsigned)s_pending_uuid16, error->status);
        s_err = "write failed";
        engine_feed(GE_ERROR, 0, NULL, 0);
        return 0;
    }
    engine_feed(GE_WRITE_OK, s_pending_uuid16, NULL, 0);
    return 0;
}

/* ---------------- hops onto the host task ---------------- */

static void on_start_req(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_req_pending) return;
    s_req_pending = false;
    /* s_active covers an attempt in flight; s_phase covers the tail of one
     * (a scan restart still being retried), during which starting a new
     * attempt would stop scanning again and burn the retry budget. */
    if (s_active || s_phase != TP_IDLE) return;

    /* The hub has ONE outbound connection (CONFIG_BT_NIMBLE_MAX_CONNECTIONS
     * = 1) and two independent schedulers wanting it. Checked here, on the
     * host task, immediately before connecting rather than back at request
     * time, so the window between the check and the connect is as small as
     * this design can make it. Dropping the request costs nothing: the
     * device will advertise again and gatt_sched_due() will still say yes.
     * Crucially it is dropped WITHOUT recording a failure -- the device did
     * nothing wrong, and a backoff here would punish it for the hub's own
     * scheduling. */
    if (s_conn_busy && s_conn_busy()) {
        ESP_LOGD(TAG, "another connection owner has the radio; dropping this request");
        return;
    }

    gatt_fsm_init(&s_fsm, s_plan, s_plan_len);
    if (s_fsm.read_count == 0) {
        ESP_LOGW(TAG, "wrapper %u: connect plan declares no reads, nothing to do",
                 (unsigned)s_wrapper_id);
        return;   /* no radio touched, so nothing to unwind and no scan to resume */
    }

    s_active = true;
    s_own_conn_proc = false;
    s_decode_wrote = false;
    s_err = "unknown error";
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_timer_stop(s_deadline);
    esp_err_t terr = esp_timer_start_once(s_deadline, (uint64_t)GATT_ATTEMPT_DEADLINE_MS * 1000);
    if (terr != ESP_OK) {
        /* Without a deadline the attempt is unbounded, and an unbounded
         * attempt is an unbounded scanning outage. Refuse to start rather
         * than run one. */
        ESP_LOGE(TAG, "deadline timer failed to arm (%d); skipping this attempt", (int)terr);
        s_active = false;
        return;
    }
    s_phase = TP_ATTEMPT;

    ESP_LOGD(TAG, "gatt attempt: dev=%d wrapper=%u reads=%u writes=%u",
             s_dev_idx, (unsigned)s_wrapper_id, (unsigned)s_fsm.read_count,
             (unsigned)s_fsm.write_count);
    engine_feed(GE_START, 0, NULL, 0);
}

static void on_deadline(struct ble_npl_event *ev)
{
    (void)ev;
    switch (s_phase) {

    case TP_ATTEMPT:
        if (!s_active) { s_phase = TP_IDLE; return; }
        ESP_LOGW(TAG, "attempt deadline (%d ms) expired in state %d",
                 GATT_ATTEMPT_DEADLINE_MS, (int)s_fsm.state);
        s_err = "timed out";
        s_phase = TP_TEARDOWN;
        engine_feed(GE_TIMEOUT, 0, NULL, 0);
        if (!s_active) return;   /* the teardown completed synchronously */
        /* Still open means a disconnect (or a connect cancel) has been
         * commanded and its confirmation is outstanding -- bound that wait
         * too, see GATT_TEARDOWN_GRACE_MS. Same rigour as on_start_req()'s
         * arming check (fix round 1): if this fails to arm, the only
         * remaining exit is the very confirmation it was meant to bound, so
         * close the attempt now rather than gamble on it arriving. */
        esp_timer_stop(s_deadline);   /* see resume_scan_or_retry() on why every arm stops first */
        if (esp_timer_start_once(s_deadline, (uint64_t)GATT_TEARDOWN_GRACE_MS * 1000) != ESP_OK) {
            ESP_LOGE(TAG, "teardown grace timer failed to arm; closing the attempt now");
            force_close();
        }
        return;

    case TP_TEARDOWN:
        if (!s_active) { s_phase = TP_IDLE; return; }
        ESP_LOGW(TAG, "teardown confirmation never arrived; forcing the attempt closed");
        force_close();
        return;

    case TP_SCAN_RETRY:
        resume_scan_or_retry();
        return;

    default:
        return;
    }
}

static void on_decoded(struct ble_npl_event *ev)
{
    (void)ev;
    /* No staleness check needed, unlike battery_poll.c's poll generation:
     * the decode runs on adv_decoder_task and gatt_engine_request() cannot
     * start a new attempt while s_decode_pending is set (gatt_engine_busy()),
     * and both run on that same task in sequence -- so a GE_DECODED can
     * never belong to an attempt other than the current one. If the current
     * one was meanwhile abandoned (a deadline during the decode), it is in a
     * terminal state and gatt_fsm_step() ignores this outright. */
    engine_feed(GE_DECODED, 0, NULL, 0);
}

static void deadline_timer_cb(void *arg)
{
    (void)arg;
    /* esp_timer task: never step the state machine from here (the NimBLE
     * host task owns it) and never make a radio call. Just hop over. */
    post_to_host(&s_ev_deadline);
}

/* ---------------- public API ---------------- */

void gatt_engine_init(void)
{
    if (s_inited) return;

    const esp_timer_create_args_t targs = { .callback = deadline_timer_cb, .name = "gatt_deadline" };
    esp_err_t err = esp_timer_create(&targs, &s_deadline);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deadline timer create failed (%d); GATT reads disabled", (int)err);
        return;
    }
    /* Requires nimble_port_init() to have run: these allocate through the
     * npl function table the port installs there. */
    ble_npl_event_init(&s_ev_start, on_start_req, NULL);
    ble_npl_event_init(&s_ev_deadline, on_deadline, NULL);
    ble_npl_event_init(&s_ev_decoded, on_decoded, NULL);

    for (int i = 0; i < GATT_SCHED_MAX_DEVICES; i++) s_last_error[i] = NULL;
    s_phase = TP_IDLE;
    s_inited = true;
}

void gatt_engine_set_scan_resume(gatt_scan_resume_fn_t fn)
{
    s_resume_scan = fn;
}

void gatt_engine_set_conn_busy_hook(gatt_conn_busy_fn_t fn)
{
    s_conn_busy = fn;
}

bool gatt_engine_busy(void)
{
    return s_active || s_req_pending || s_decode_pending || s_phase == TP_SCAN_RETRY;
}

void gatt_engine_request(uint16_t wrapper_id, int dev_idx, const uint8_t mac[6],
                         uint8_t addr_type)
{
    if (!s_inited) return;
    if (dev_idx < 0 || dev_idx >= GATT_SCHED_MAX_DEVICES) return;
    if (gatt_engine_busy()) return;   /* spec section 7: dropped, never queued */

    /* Copied out of the wrapper arena here, on adv_decoder_task, because
     * this is the only task allowed to touch flash -- and because an arena
     * pointer can be invalidated by any later eviction, while
     * gatt_fsm_t.write[].data will be dereferenced much later, on the
     * NimBLE host task. */
    uint16_t n = wrapper_exec_plan_get(wrapper_id, s_plan, sizeof s_plan, NULL);
    if (n == 0) return;

    s_plan_len = n;
    s_wrapper_id = wrapper_id;
    s_dev_idx = dev_idx;
    memcpy(s_mac_gap, mac, 6);
    for (int i = 0; i < 6; i++) s_mac_disp[i] = mac[5 - i];
    s_addr_type = addr_type;

    s_req_pending = true;   /* set LAST: it is what makes everything above visible to on_start_req() */
    post_to_host(&s_ev_start);
}

void gatt_engine_service(void)
{
    if (!s_decode_pending) return;

    /* The wrapper VM and the flash-backed arena, on the task that is
     * allowed to use both. The buffer is gatt_fsm_t's own concatenated
     * read buffer -- PSVM_PLAN_MAX_READS fixed 16-byte slots, which is
     * exactly the layout the compiler turned each named buffer into a
     * constant offset within (spec section 2). */
    s_decode_wrote = wrapper_exec_run_buffer(s_wrapper_id, s_mac_disp,
                                             s_fsm.buf, s_decode_len);
    s_decode_pending = false;
    post_to_host(&s_ev_decoded);
}

const char *gatt_engine_last_error(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= GATT_SCHED_MAX_DEVICES) return "";
    const char *e = s_last_error[dev_idx];
    return e ? e : "";
}
