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
#include "event_log.h"
#include "psvm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "gatt_engine";

/* Design spec section 3's hard deadline. PER ATTEMPT, not per operation:
 * one timer is armed when the attempt starts and disarmed when it ends by
 * any route. A per-operation timer would let a device that answers every
 * single step slowly hold the radio -- and with it the hub's entire
 * advertisement intake, since scanning is stopped while connected --
 * indefinitely without ever technically timing out. */
#define GATT_ATTEMPT_DEADLINE_MS 5000

/* Not a second deadline: this is armed only AFTER the attempt deadline has
 * already expired and a disconnect has been commanded, to bound the wait
 * for the BLE_GAP_EVENT_DISCONNECT that confirms it. Without it, a
 * teardown whose confirmation never arrives would leave s_active true
 * forever, and since restarting the scan is part of ending an attempt, the
 * hub would stay deaf to every advertisement for the rest of the boot --
 * the exact failure the deadline exists to prevent, reintroduced one step
 * later. battery_poll.c's POLL_WATCHDOG_S guards the same hazard. */
#define GATT_TEARDOWN_GRACE_MS 1000

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

/* ---------------- state ----------------
 *
 * All of this is static, per spec section 6's budget: one attempt at a time
 * is the same single-writer argument the shared read buffer rests on.
 *
 * Unless a field's comment says otherwise it is owned by the NimBLE host
 * task and touched nowhere else (see gatt_engine.h's task-ownership note).
 */
static gatt_fsm_t s_fsm;
static bool     s_active;          /* an attempt is open: from start_attempt() to attempt_finish() */
static bool     s_grace;           /* the deadline already fired; the timer is now the teardown watchdog */
static uint16_t s_wrapper_id;
static int      s_dev_idx = -1;
static uint8_t  s_mac_gap[6];      /* raw GAP/on-air order -- what ble_gap_connect() wants */
static uint8_t  s_mac_disp[6];     /* display/human order -- what wrapper_exec_run_buffer() wants */
static uint8_t  s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_pending_uuid16;  /* the characteristic the in-flight read/write is for */
static uint16_t s_pending_handle;  /* ...and the ATT handle it resolved to */
static uint8_t  s_plan[GATT_PLAN_MAX];
static uint16_t s_plan_len;

/* Why the failure reason is a const char * and not a formatted buffer:
 * spec section 6 budgets ~200 B for the whole connection manager, and 16
 * devices x even a modest fixed-size string is several times that. Every
 * value ever stored here is a string literal chosen at the point of
 * failure, so a per-device pointer table costs 64 B and keeps the
 * budget honest. The NimBLE status code that a formatted string would have
 * carried is not lost -- it goes to the log line at the point of failure,
 * where it is actually useful for diagnosis, rather than into a UI field
 * whose job is to say WHAT failed in words a user can act on. */
static const char *s_last_error[GATT_SCHED_MAX_DEVICES];
static const char *s_err;          /* reason for the failure currently being processed */

/* Set by the host task, consumed by adv_decoder_task (gatt_engine_service()).
 * Plain volatile bools, exactly like ble_collector.c's
 * s_wrapper_reindex_pending: one bit of information each, nothing else
 * depends on their ordering relative to anything but the fields they gate,
 * and each is written by one task and cleared by the other. */
static volatile bool s_decode_pending;
static volatile bool s_report_pending;
static volatile bool s_req_pending;    /* the other direction: decoder task -> host task */
static uint8_t  s_decode_len;
static bool     s_decode_wrote;        /* did the decode actually write a capability value */

/* Snapshot of the finished attempt, for the deferred event-log entry.
 * event_log_append() writes LittleFS and then runs the SSE and MQTT hooks,
 * so it can never be called from the NimBLE host task; attempt_finish()
 * fills this in instead and adv_decoder_task writes the entry. */
static bool     s_report_ok;
static const char *s_report_err;
static uint8_t  s_report_mac[6];
static uint8_t  s_report_fails;

static esp_timer_handle_t s_deadline;
static struct ble_npl_event s_ev_start;
static struct ble_npl_event s_ev_deadline;
static struct ble_npl_event s_ev_decoded;
static gatt_scan_resume_fn_t s_resume_scan;
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

/* ---------------- the one place an attempt ends ----------------
 *
 * SUCCESS, read error, timeout, an unsolicited disconnect, a connect that
 * never established, a NimBLE call refused at issue time -- every route out
 * of an attempt passes through here, and this function ALWAYS restarts
 * scanning. That is the whole reason it exists as a single function rather
 * than as a line in each callback: a hub that stops scanning after a failed
 * connection is worse than one that never connected, because it goes
 * silently deaf to every advertisement afterwards -- including the ones
 * that would have triggered a retry. There is no path that ends an attempt
 * without calling this, and this cannot run without resuming the scan.
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
    s_grace = false;

    /* The one fact only the state machine knows: which terminal state this
     * attempt reached. GS_DONE is reached solely via GE_DECODED, i.e. every
     * declared read landed and the decode ran. */
    bool ok = (s_fsm.state == GS_DONE);
    uint32_t now = now_s();

    if (ok) {
        gatt_sched_ok(s_dev_idx, now);
    } else {
        gatt_sched_fail(s_dev_idx, now);
        /* Design spec section 4: a failed attempt is precisely the right
         * trigger to rediscover next time -- a device firmware update is
         * the likeliest reason a cached handle stopped working, and
         * rediscovery is the only thing that fixes it. */
        gatt_cache_drop(s_dev_idx);
    }

    if (s_dev_idx >= 0 && s_dev_idx < GATT_SCHED_MAX_DEVICES) {
        if (!ok) {
            s_last_error[s_dev_idx] = s_err;
        } else if (!s_decode_wrote) {
            /* Radio-wise this attempt worked, so it does NOT count as a
             * failure (no backoff, no cache drop: the handles are demonstrably
             * fine). But spec section 5 exists so that a connect block
             * contributing nothing is visible rather than silently installed
             * and enabled, so the device says so. */
            s_last_error[s_dev_idx] = "read ok, decode emitted nothing";
        } else {
            s_last_error[s_dev_idx] = NULL;
        }
    }

    s_report_ok = ok;
    s_report_err = ok ? "" : (s_err ? s_err : "unknown error");
    memcpy(s_report_mac, s_mac_disp, 6);
    s_report_fails = gatt_sched_fail_count(s_dev_idx);
    s_report_pending = true;

    s_active = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    if (s_resume_scan) {
        s_resume_scan();
    } else {
        ESP_LOGE(TAG, "no scan-resume hook installed: scanning is NOT running and this hub "
                      "is now deaf to advertisements");
    }
}

/* Defined below; declared here because perform() hands each of them to
 * NimBLE as the callback for the operation it issues. */
static int gatt_gap_event(struct ble_gap_event *event, void *arg);
static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
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
        /* NimBLE gets the same 5-second budget as our own deadline, so a
         * peer that never answers the connection request is reported by the
         * stack itself (a CONNECT event with a nonzero status) instead of
         * only ever by our timer. Our timer remains the backstop. */
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, GATT_ATTEMPT_DEADLINE_MS,
                             NULL, gatt_gap_event, NULL);
        if (rc != 0) {
            ESP_LOGI(TAG, "ble_gap_connect refused: %d", rc);
            s_err = "connect refused";
            return true;
        }
        return false;
    }

    case GA_DISCOVER: {
        int rc = ble_gattc_disc_all_chrs(s_conn_handle, 1, 0xffff, disc_chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGI(TAG, "disc_all_chrs refused: %d", rc);
            s_err = "discovery refused";
            return true;
        }
        return false;
    }

    case GA_WRITE:
    case GA_READ: {
        /* The state machine names characteristics by uuid16 and never sees
         * a handle (spec section 4: handles are never written into a
         * wrapper, and gatt_fsm.c holds itself to the same discipline).
         * Resolving uuid16 to an ATT handle is this layer's job. */
        uint16_t h = gatt_cache_lookup(s_dev_idx, a->uuid16);
        if (h == 0) {
            ESP_LOGI(TAG, "no handle cached for uuid16 0x%04X", (unsigned)a->uuid16);
            s_err = "characteristic not found";
            return true;
        }
        s_pending_uuid16 = a->uuid16;
        s_pending_handle = h;
        int rc;
        if (a->kind == GA_READ) {
            rc = ble_gattc_read(s_conn_handle, h, read_cb, NULL);
        } else {
            /* The plan's optional pre-read write is the ONLY write M5a
             * performs; actuators and any other state-changing write are
             * M5b (spec section 9). a->data points into s_plan, which
             * outlives this call. */
            rc = ble_gattc_write_flat(s_conn_handle, h, a->data, a->len, write_cb, NULL);
        }
        if (rc != 0) {
            ESP_LOGI(TAG, "%s of 0x%04X refused: %d",
                     a->kind == GA_READ ? "read" : "write", (unsigned)a->uuid16, rc);
            s_err = (a->kind == GA_READ) ? "read refused" : "write refused";
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
        int rc = BLE_HS_ENOTCONN;
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (rc != 0) {
            /* There is no link to close -- a connect that never established,
             * or one already gone. No BLE_GAP_EVENT_DISCONNECT will ever
             * arrive, so ending the attempt here is the only way scanning
             * comes back. */
            attempt_finish();
        }
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

/* ---------------- NimBLE callbacks (all on the host task) ---------------- */

static int gatt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (!s_active) {
                /* The attempt this connection belongs to was already forced
                 * closed (a teardown that never confirmed). Nothing owns
                 * this link, and with CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1 an
                 * unowned link would block every future attempt, so close
                 * it rather than leak it. */
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            engine_feed(GE_CONNECTED, 0, NULL, 0);
        } else {
            /* A connect failure and NimBLE's own connect timeout both land
             * here with the same nonzero status, and neither leaves a link
             * behind. */
            ESP_LOGI(TAG, "connect failed/timed out: %d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_err = "connect failed";
            engine_feed(GE_ERROR, 0, NULL, 0);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Ignore a disconnect belonging to a link this engine has already
         * stopped tracking -- see stale_completion() for the same argument
         * applied to the GATT callbacks. */
        if (!s_active || event->disconnect.conn.conn_handle != s_conn_handle) return 0;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_fsm.state != GS_DONE && s_fsm.state != GS_FAILED) {
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

/* True once every uuid16 this plan needs -- reads AND the optional
 * pre-read writes -- has a cached handle for this device. Used twice: to
 * decide whether discovery can be skipped entirely (spec section 4's warm
 * cache), and to decide whether a discovery that just finished actually
 * found what the plan asked for. */
static bool have_all_handles(void)
{
    for (uint8_t i = 0; i < s_fsm.read_count; i++) {
        if (gatt_cache_lookup(s_dev_idx, s_fsm.read_uuid[i]) == 0) return false;
    }
    for (uint8_t i = 0; i < s_fsm.write_count; i++) {
        if (gatt_cache_lookup(s_dev_idx, s_fsm.write[i].uuid16) == 0) return false;
    }
    return true;
}

/* Does this plan actually name uuid16? Discovery enumerates EVERY
 * characteristic the peer exposes, while the cache holds exactly four
 * entries (gatt_sched.h's GATT_CACHE_MAX_ENTRIES, sized to the 4-read plan
 * cap) and refuses a fifth distinct uuid16 rather than evicting. Storing
 * indiscriminately would therefore fill the cache with the first four
 * characteristics the device happens to declare -- typically Device Name and
 * Appearance from the mandatory GAP service -- and then refuse the ones the
 * plan actually needs. Filtering here is not an optimisation; without it a
 * warm cache would be permanently wrong on any device with more than four
 * characteristics. */
static bool plan_wants(uint16_t uuid16)
{
    for (uint8_t i = 0; i < s_fsm.read_count; i++) {
        if (s_fsm.read_uuid[i] == uuid16) return true;
    }
    for (uint8_t i = 0; i < s_fsm.write_count; i++) {
        if (s_fsm.write[i].uuid16 == uuid16) return true;
    }
    return false;
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

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (stale_completion(conn_handle)) return 0;

    if (error->status == 0 && chr != NULL) {
        /* Only 16-bit UUIDs are addressable from a plan at all (spec
         * section 2: characteristics are named by 16-bit UUID, which is what
         * a datasheet gives and what survives a device firmware update), so
         * a 128-bit characteristic is simply not this plan's business. */
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            uint16_t u = ble_uuid_u16(&chr->uuid.u);
            if (plan_wants(u)) gatt_cache_store(s_dev_idx, u, chr->val_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!have_all_handles()) {
            /* The device does not expose something the plan names. Failing
             * here rather than letting GA_READ discover it means the
             * attempt reports one honest reason instead of a per-read one,
             * and the cache drop that attempt_finish() performs clears the
             * partial results this discovery just stored. */
            ESP_LOGI(TAG, "discovery finished without every characteristic the plan needs");
            s_err = "characteristic not found";
            engine_feed(GE_ERROR, 0, NULL, 0);
            return 0;
        }
        engine_feed(GE_DISCOVERED, 0, NULL, 0);
        return 0;
    }

    ESP_LOGI(TAG, "discovery failed: %d", error->status);
    s_err = "discovery failed";
    engine_feed(GE_ERROR, 0, NULL, 0);
    return 0;
}

static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (stale_completion(conn_handle)) return 0;

    if (error->status != 0 || attr == NULL) {
        ESP_LOGI(TAG, "read of 0x%04X failed: %d", (unsigned)s_pending_uuid16, error->status);
        s_err = "read failed";
        engine_feed(GE_ERROR, 0, NULL, 0);
        return 0;
    }
    /* Adapter-layer identity filter, in the ATT handle space NimBLE speaks.
     * gatt_fsm.c performs the authoritative check one layer up, in the
     * uuid16 space the plan speaks (gatt_ev_t.handle's doc comment explains
     * why that check has to live where a host test can reach it), and this
     * is the defence-in-depth half it explicitly permits. The translation
     * between the two spaces happens right here: what goes into the event
     * is the uuid16 we asked for, never the ATT handle NimBLE answered
     * with, because the state machine has never seen a handle in its life. */
    if (attr->handle != s_pending_handle) {
        ESP_LOGD(TAG, "read completion for handle %u, expected %u -- ignored",
                 (unsigned)attr->handle, (unsigned)s_pending_handle);
        return 0;
    }

    uint8_t buf[GATT_FSM_SLOT];
    uint16_t n = OS_MBUF_PKTLEN(attr->om);
    if (n > GATT_FSM_SLOT) n = GATT_FSM_SLOT;   /* a longer value is truncated to its slot */
    if (n > 0 && os_mbuf_copydata(attr->om, 0, n, buf) != 0) {
        s_err = "read failed";
        engine_feed(GE_ERROR, 0, NULL, 0);
        return 0;
    }
    /* A short read zero-pads its slot inside gatt_fsm_step(), which
     * re-zeroes the slot before copying -- no previous device's bytes can
     * survive in a slot this attempt did not fill. */
    engine_feed(GE_READ_OK, s_pending_uuid16, buf, (uint8_t)n);
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
    if (s_active) return;   /* cannot happen: gatt_engine_request() gates on busy */

    /* Parsed twice, deliberately. gatt_fsm_init() is what knows the plan's
     * on-blob layout, and have_all_handles() needs the parsed uuid16s to
     * answer the question gatt_fsm_init() itself takes as an argument. The
     * alternative -- a second plan parser here, in the one file no host test
     * can reach -- is exactly the duplication spec section 4 and gatt_fsm.h
     * spent their effort avoiding. Parsing a 36-byte section twice costs
     * nothing and happens at most once per declared interval. */
    gatt_fsm_init(&s_fsm, s_plan, s_plan_len, false);
    if (s_fsm.read_count == 0) {
        ESP_LOGW(TAG, "wrapper %u: connect plan declares no reads, nothing to do",
                 (unsigned)s_wrapper_id);
        return;   /* no radio touched, so nothing to unwind and no scan to resume */
    }
    bool warm = have_all_handles();
    gatt_fsm_init(&s_fsm, s_plan, s_plan_len, warm);

    s_active = true;
    s_grace = false;
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

    ESP_LOGD(TAG, "gatt attempt: dev=%d wrapper=%u reads=%u writes=%u warm=%d",
             s_dev_idx, (unsigned)s_wrapper_id, (unsigned)s_fsm.read_count,
             (unsigned)s_fsm.write_count, (int)warm);
    engine_feed(GE_START, 0, NULL, 0);
}

static void on_deadline(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_active) return;

    if (!s_grace) {
        ESP_LOGW(TAG, "attempt deadline (%d ms) expired in state %d",
                 GATT_ATTEMPT_DEADLINE_MS, (int)s_fsm.state);
        s_err = "timed out";
        s_grace = true;
        engine_feed(GE_TIMEOUT, 0, NULL, 0);
        /* Still open means a disconnect has been commanded and its
         * confirmation is outstanding -- bound that wait too, see
         * GATT_TEARDOWN_GRACE_MS. */
        if (s_active) {
            esp_timer_start_once(s_deadline, (uint64_t)GATT_TEARDOWN_GRACE_MS * 1000);
        }
        return;
    }

    ESP_LOGW(TAG, "teardown confirmation never arrived; forcing the attempt closed");
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    attempt_finish();
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
    s_inited = true;
}

void gatt_engine_set_scan_resume(gatt_scan_resume_fn_t fn)
{
    s_resume_scan = fn;
}

bool gatt_engine_busy(void)
{
    return s_active || s_req_pending || s_decode_pending || s_report_pending;
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
    if (s_decode_pending) {
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

    if (s_report_pending) {
        /* Deferred off the NimBLE host task on purpose: event_log_append()
         * writes a LittleFS record and then runs the SSE and MQTT hooks.
         * Stack-wise this is the same order of magnitude as the LittleFS
         * read chain this task already runs on every arena miss
         * (wrapper_arena_get -> wrapper_store_read_psbc -> fopen/fread), and
         * it is never nested inside the decode above -- they are sequential
         * statements, not a call chain -- so the task's peak does not stack
         * the two. Spec section 5: every attempt, success and failure,
         * writes to the event log, so the Rules tab's feed shows connection
         * history without a new UI surface. */
        char msg[EVENT_MSG_MAX + 1];
        if (s_report_ok) {
            snprintf(msg, sizeof msg, "GATT read ok: %02X:%02X:%02X:%02X:%02X:%02X",
                     s_report_mac[0], s_report_mac[1], s_report_mac[2],
                     s_report_mac[3], s_report_mac[4], s_report_mac[5]);
        } else {
            snprintf(msg, sizeof msg,
                     "GATT read failed: %02X:%02X:%02X:%02X:%02X:%02X -- %s (fail #%u)",
                     s_report_mac[0], s_report_mac[1], s_report_mac[2],
                     s_report_mac[3], s_report_mac[4], s_report_mac[5],
                     s_report_err, (unsigned)s_report_fails);
        }
        event_log_append(0, 0, msg);
        s_report_pending = false;
    }
}

const char *gatt_engine_last_error(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= GATT_SCHED_MAX_DEVICES) return "";
    const char *e = s_last_error[dev_idx];
    return e ? e : "";
}
