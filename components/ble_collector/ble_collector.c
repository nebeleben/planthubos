#include "ble_collector.h"
#include "data_core.h"
#include "mibeacon.h"
#include "bthome.h"
#include "battery_sched.h"
#include "ble_collector_internal.h"
#include "adv_queue.h"
#include "swarm_store.h"
#include "rules.h"
#include "wrapper_index.h"
#include "wrapper_arena.h"
#include "wrapper_exec.h"
#include "unknown_capture.h"
#include "gatt_engine.h"
#include "gatt_sched.h"
#include "actor.h"
#include "actor_persist.h"
#include "alert.h"
#include "event_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include <string.h>

static const char *TAG = "ble_collector";

#define XIAOMI_SVC_UUID 0xFE95
/* GAP scan units are 0.625 ms */
#define SCAN_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

/* M5a gate fix (spec §2, amended 2026-08-17): bounds the fallback walk over
 * a connectable peripheral's advertised 16-bit Service Class UUID list
 * (fields.uuids16, AD types 0x02/0x03) in decode_adv_item() below. A legacy
 * advert is at most ADV_PAYLOAD_MAX=31 bytes total, so num_uuids16 can never
 * exceed roughly 14 in the most degenerate case (the whole advert being one
 * UUID16-list structure and nothing else); 8 is a cheap, generous cap for a
 * per-advertisement scan on the decoder task -- order is the advertiser's
 * choice, so the first ACCEPTED match wins, not the last. */
#define ADV_UUID16_SCAN_MAX 8

/* M3 §1 raw-advert pipeline: gap_event (NimBLE host task) copies the raw
 * advertisement into this ring and returns; adv_decoder_task drains it and
 * does the actual decode (parsing, MiBeacon today, wrapper/BTHome dispatch
 * from Task 2 onward) plus every flash/registry access that decode implies.
 * The ring itself is a plain static -- 16 * 48 B budgeted (M3 spec §7),
 * sizeof(adv_ring_t) here is 712 B (16 * 44 B items + 8 B of head/tail/
 * full/dropped bookkeeping) -- rather than a heap-backed FreeRTOS queue
 * object, which would need its own separately allocated buffer for the same
 * data. s_adv_mux is a spinlock (portMUX_TYPE), not a SemaphoreHandle_t: the
 * producer (gap_event) must never block (see its own comment), and a
 * critical section around a single struct-copy push/pop is a few
 * instructions -- exactly the case portENTER_CRITICAL/portEXIT_CRITICAL
 * exist for, unlike s_batt_mutex below which guards a longer-lived table
 * across two independent tasks and can afford to take a real mutex. */
static adv_ring_t s_adv_ring;
static portMUX_TYPE s_adv_mux = portMUX_INITIALIZER_UNLOCKED;

/* M3 Task 2 (spec §2): the match index, built once at boot by
 * wrapper_store_load_all() (below, in ble_collector_start(), before the
 * decoder task or NimBLE scanning can start) and read-only from then on --
 * only Task 7's install/delete API will ever mutate it again (a later
 * task). 16 * 12 B = 192 B static, exactly the spec §7 budget line;
 * adv_decoder_task is the only reader, so this needs no lock of its own
 * (same reasoning as s_batt_tab needing one and this not: unlike s_batt_tab,
 * nothing outside the decoder task touches this table in M3 Task 2). */
static wrapper_index_t s_wrapper_index;

/* M3 Task 5 (spec §2 "device -> wrapper memo"): one entry per registry
 * device, the id of the wrapper that last matched it -- so a repeat
 * advertisement from a device already known to match a specific wrapper
 * skips wrapper_index_lookup() entirely and goes straight to
 * wrapper_arena_get()+wrapper_exec_run(). WRAPPER_MEMO_NONE means "no memo
 * recorded" -- NOT "confirmed no wrapper matches"; a device whose last
 * advert genuinely matched nothing always re-runs a real lookup, which is
 * cheap (an O(WRAPPERS_MAX) scan) and, unlike a positive match, has nothing
 * worth caching per spec's own wording ("the wrapper id that LAST
 * MATCHED"). Indexed by registry slot (data_core_find_index()), not by MAC
 * -- see that function's own doc comment for why a registry index is
 * stable for a device's lifetime.
 *
 * M3 review fix 4: this was originally uint8_t with WRAPPER_MEMO_NONE=0xFF,
 * but wrapper ids are a monotonic, never-reused uint16_t counter
 * (wrapper_store.c's s_wrapper_next_id) -- NOT a 0..WRAPPERS_MAX-1 slot
 * index -- so a long-lived hub that has ever CREATED (not just currently
 * has) 255 wrappers would see id 255 alias the sentinel and id 256 memoise
 * as 0, silently feeding wrapper_exec_run() the wrong wrapper forever for
 * that device (ble_collector.c's own dispatch just below). Widened to
 * uint16_t -- matching wrapper_exec_run()'s own id type exactly, so the
 * memo can represent every id that type can ever hold, not just a
 * corner-case-prone subset -- rather than refusing to memoise past 254
 * (which would need its own bookkeeping and just moves the same class of
 * cliff from 255 to 65535 creations). Costs REGISTRY_MAX_DEVICES extra
 * bytes (16 B, one per device: 16*2 B = 32 B vs the previous 16 B) against
 * the C3's ~1.4 KB RAM margin over its spec floor -- about 1% of that
 * margin, for correctness across the id type's entire real range instead
 * of failing after ~255 wrapper creations. */
#define WRAPPER_MEMO_NONE 0xFFFFu
static uint16_t s_wrapper_memo[REGISTRY_MAX_DEVICES];
/* Per-device connect-plan interval, 0 meaning "no plan" -- written ONLY by
 * adv_decoder_task, read by the httpd and SSE tasks through
 * ble_collector_plan_interval_for_device().
 *
 * This exists because GET /api/v1/devices must not reach the wrapper arena.
 * devices_json.c originally answered its "gatt" object by calling
 * wrapper_exec_plan_get(), which calls wrapper_arena_get() -- and the arena
 * has no lock at all, because ble_collector.h's reindex-request comment
 * makes decoder-task exclusivity the arena's whole concurrency argument. An
 * arena miss evicts and memmove()s the backing buffer; the httpd task (prio
 * 5) and the SSE event-loop task both preempt the decoder task (prio 3), so
 * a Devices-tab poll could relocate bytecode underneath a psvm_run() that
 * was executing it -- memory corruption, not a stale read. The miss path
 * also loads from LittleFS, which does not belong on the event loop's
 * 2304 B stack (the same reasoning that cut the event-log write from M5a,
 * spec section 5).
 *
 * decode_adv_item() already computes this value on the decoder task for its
 * own scheduling decision, so memoising it costs one store and no extra
 * lookup. Single writer, naturally-aligned uint32_t elements: a concurrent
 * reader sees either the previous advertisement's value or this one, never
 * a torn one -- the same argument s_wrapper_memo's own doc comment makes.
 * A reindex memset()s this table alongside that one, so the same
 * byte-mixed-read caveat that comment records applies here too; the
 * consequence is bounded the same way (every byte written is 0x00, so the
 * mixed value is still a plausible interval at worst) and self-corrects on
 * the next advertisement. */
static uint32_t s_plan_interval_memo[REGISTRY_MAX_DEVICES];

/* ---------------- M5b actuators: the composition this file owns ----------
 *
 * components/actors knows WHETHER a command may run (guards, TTL, queue
 * priority) and components/gatt knows HOW to put it on the air. Neither may
 * depend on the other -- actor.c calls into the engine and the engine must
 * report back, and a REQUIRES in both directions is an ESP-IDF component
 * cycle. So the two halves are joined HERE, by the component that already
 * owns the composition, through the NULL-safe hooks each side exposes
 * (actor_set_dispatch_hook() / gatt_engine_set_cmd_done_hook()) -- the same
 * pattern this file already uses for gatt_engine_set_scan_resume() and that
 * rules_engine.c uses for alert.h's wake hook.
 *
 * What only this file can supply is the translation between the two: an
 * actor_cmd_t names a registry index and an action id, while a connection
 * needs a GAP address, an address type and the wrapper's declarative action
 * entry. The address type in particular exists NOWHERE else -- the registry
 * stores a device id, not how to connect to it -- so it is captured from
 * the advertisement that proved the device exists, which is also the only
 * moment it is ever observable.
 *
 * Sized to ACTOR_MAX_DEVICES rather than REGISTRY_MAX_DEVICES: only a
 * device with declared actions can ever be commanded, and the actor table
 * itself holds four (spec section 1's "a plant hub drives a few valves, not
 * sixteen"). 4 x 8 B, against a 16-entry table's 112 B. */
typedef struct {
    int8_t   dev_idx;      /* -1 when free */
    uint8_t  addr_type;
    uint8_t  mac[6];       /* RAW GAP/on-air order -- what ble_gap_connect() wants */
    uint16_t wrapper_id;   /* the wrapper whose action table bound this device */
} actor_conn_t;
static actor_conn_t s_actor_conn[ACTOR_MAX_DEVICES];

/* One bit per registry index: "this device's wrapper has already been asked
 * whether it declares actions". Without it, every advertisement from every
 * non-actuator device would pay an arena load plus a psvm_validate() to be
 * told "no" again. Cleared with the wrapper memo on a reindex, since an
 * installed or deleted wrapper changes the answer. */
static uint16_t s_actor_asked;
_Static_assert(REGISTRY_MAX_DEVICES <= 16, "s_actor_asked is a 16-bit index bitmap");

/* The command currently in flight, remembered here because
 * gatt_cmd_done_fn_t deliberately carries only (dev_idx, ok, confirmed,
 * err) -- everything else it would have to carry is knowledge of the actor
 * layer, which is exactly what must not cross that boundary.
 *
 * Written by adv_decoder_task at dispatch, read by the NimBLE host task in
 * the completion hook, with no lock -- safe because the two can never
 * overlap, and it is worth being exact about why rather than resting on
 * "there is only one": the engine calls the hook from inside
 * attempt_finish() BEFORE it clears s_active, so gatt_engine_cmd_busy() is
 * still true for the whole duration of the hook, and the decoder loop will
 * not dispatch the next command (and so will not touch this struct) until
 * it goes false. */
static actor_cmd_t s_cmd_inflight = { .dev_idx = -1, .action_id = ACTION_NONE };

/* The re-queue path for a command the GATT engine refused because the OTHER
 * owner of the hub's single outbound connection had the radio
 * (GATT_CMD_ERR_RADIO_BUSY). By then actor_service_step() has already
 * popped that command and charged it against its hourly budget, and nothing
 * else would ever put it back -- an irrigation-open lost because the
 * MiFlora poller happened to be connecting would simply be gone (fix round
 * 1, Critical 2).
 *
 * This is now the BACKSTOP rather than the main defence (fix round 2): the
 * decoder loop no longer pops a command at all while battery_poll_busy()
 * says the radio is taken, so the collision this recovers from has shrunk
 * to the microseconds between that test and the engine's own check inside
 * start_command(). Rare instead of deterministic -- which matters, because
 * a re-queued command faces the guards again with its own activation
 * already charged, so on a pair with a cooldown the retry is refused by
 * that cooldown. Narrowing the window is what makes that residual
 * acceptable; it is not a reason to remove the backstop, since the window
 * is real.
 *
 * Performed on adv_decoder_task, never from the completion hook itself:
 * that hook runs on the NimBLE host task for a radio outcome, and
 * actor_request_retry() takes the actor mutex. Same discipline, same
 * reason, as the switch.state write gatt_engine_service() defers to this
 * task.
 *
 * ONCE per command -- carried by actor_cmd_t.retried, which travels WITH
 * the command through the queue (see its own doc comment for why a
 * single-slot latch here was wrong). Unbounded retries are not bounded by
 * the TTL alone: every dispatch re-runs actor_table_record(), so a command
 * bouncing off a busy radio once per decoder tick would spend a
 * max_per_hour budget in milliseconds. */
static actor_cmd_t s_requeue;          /* the command to put back, valid iff pending */
static volatile bool s_requeue_pending;

/* M5b Task 9 fix round 1 (Critical finding 1): on_gatt_cmd_done() runs on
 * the NimBLE host task, and pending_close_clear() (via
 * pending_close_note_result()) touches LittleFS -- a blocking, flash-
 * erasing write that must never happen on that task (this file's own
 * comment on the switch.state write, just above, states the same
 * invariant; gatt_engine.c repeats it for adv_decoder_task being the only
 * task allowed to touch flash at all). So the hook only LATCHES what it
 * learned; adv_decoder_task drains and acts on it, exactly like
 * s_requeue/s_requeue_pending above and s_cmd_state_pending in
 * gatt_engine.c. Single-slot is safe for the same reason s_cmd_inflight is:
 * only one command is ever in flight, so at most one outcome is ever
 * pending here at a time. This also closes fix round 1 finding 3 (the
 * pending_close RAM table race) BY CONSTRUCTION: every mutation of that
 * table now happens only on adv_decoder_task, both here and from
 * pending_close_service().
 *
 * Whole-branch review, ruling FINAL-arm: this deferral now carries ONLY a
 * close's outcome. The ARM half is gone from here entirely -- an open's
 * obligation is armed in on_actor_dispatch(), on adv_decoder_task, before
 * the command is handed to the radio, which is both a task where flash is
 * already legal and the only point early enough to survive a post-write
 * failure or a brownout.
 *
 * Fix round 3, finding 3 (byte count updated for that removal): the four
 * statics below sum to 10 B (enum 4 + int 4 + bool 1 + bool 1), down from
 * six statics summing to 15 B -- separate statics, not one packed struct,
 * so the linker may add a few bytes of alignment padding between them;
 * 10 B is the field-size sum, not a guaranteed total object size. */
typedef enum { PC_DEFER_NONE = 0, PC_DEFER_NOTE_RESULT } pc_defer_kind_t;
static volatile pc_defer_kind_t s_pc_defer_kind;
static int      s_pc_defer_dev_idx;
static bool     s_pc_defer_ok;             /* PC_DEFER_NOTE_RESULT only */
static bool     s_pc_defer_confirmed;      /* PC_DEFER_NOTE_RESULT only */

/* Set once both hooks are registered, which happens on the HUB only -- a
 * node's radio belongs to ESP-NOW, exactly as it does for battery polling
 * and for M5a's GATT reads. Until then (and forever on a node) no device is
 * bound as an actuator and the queue is not pumped, so a command can never
 * be popped, charged against its budget and then dropped for want of
 * somewhere to send it. adv_decoder_task is created before that
 * registration, so this closes a real window, not a theoretical one. */
static bool s_actors_wired;

/* Task 5 review FINDING 4/2, corrected by M5a Task 7 fix round 1: s_wrapper_index
 * and the arena are decoder-task-exclusive, full stop -- no reader anywhere
 * else. s_wrapper_memo is NOT exclusive any more: it is single-writer,
 * multi-reader, the same shape Task 6's review made gatt_sched.h,
 * gatt_engine.h and battery_poll.c say about their own cross-task tables
 * (that review even named this file's future httpd reader in advance). The
 * SOLE WRITER stays adv_decoder_task -- the only task that ever stores a
 * match (decode_adv_item(), below) or reindexes (do_wrapper_reindex(),
 * which bulk-memset()s the whole table on install/edit/delete). The
 * READERS are adv_decoder_task itself (the per-advertisement memo check)
 * and, since M5a Task 7, the httpd task via ble_collector_wrapper_for_device()
 * (ble_collector.h) -- GET /api/v1/devices' "gatt" object calls it to learn
 * which wrapper's plan to consult for a device. A stale claim of exclusivity
 * here is exactly the defect class Task 6's review caught in the
 * neighbouring headers; left uncorrected it would tell the next reader a
 * check is unnecessary, which is worse than no comment at all.
 *
 * That second reader is not why invalidation still runs ONLY on the
 * decoder task, though -- it is because a caller that reindexed directly,
 * from a different task, while the decoder task might be mid-psvm_run()
 * over a pointer into the SAME arena these three tables gate together,
 * would be memory corruption, not merely a stale read (wrapper_arena.h's
 * FINDING 2 doc comment: wrapper_arena_get()'s returned pointer is
 * invalidated by ANY eviction from ANY caller, so a lock here would need to
 * span the ENTIRE decode, not just the index touch -- rejected for the same
 * reason a mutex around the hot per-advert path always is). So this is a
 * plain "reindex requested" flag instead: any task may set it
 * (ble_collector_wrapper_reindex_request()), and only adv_decoder_task ever
 * clears it and performs the actual reindex, at a safe point between
 * decodes (never mid-decode_adv_item()). A `volatile bool` is sufficient --
 * one bit of information, no other state depends on its ordering relative
 * to anything else, same "single aligned word, atomic enough on this
 * target" reasoning ble_collector_adv_dropped()'s own comment already gives
 * for its uint32_t. Worst-case latency to notice a request is one
 * ADV_DECODER_POLL_MS tick (20 ms) -- fine: spec section 9's own words are
 * "install takes effect on the next advertisement", not instantly. */
static volatile bool s_wrapper_reindex_pending;

#define ADV_DECODER_TASK_STACK 3072
/* Below the NimBLE host task (configMAX_PRIORITIES - 4, see
 * nimble_port_freertos_init()/esp-idf's nimble_port_freertos.c) by a wide
 * margin -- this task must never preempt radio servicing, only mop up
 * behind it. Same low-priority band as this file's own battery poller
 * (battery_poll.c, prio 3) and pairing's background tasks. */
#define ADV_DECODER_TASK_PRIO  3
/* How long adv_decoder_task sleeps when the ring is empty before checking
 * again. MiFlora/BTHome/wrapper-target advertisement intervals are all
 * >= 100 ms in practice, so this adds no perceptible latency while keeping
 * the task off the CPU between advertisements. */
#define ADV_DECODER_POLL_MS    20

/* Directly-heard sensors, candidates for the daily battery poll (see
 * battery_poll.c). Written from this file's gap_event (NimBLE host task)
 * and from battery_poll.c's poller task (a distinct FreeRTOS task) -- two
 * genuinely different tasks, so every access to s_batt_tab, on either side,
 * must hold s_batt_mutex. This mirrors data_core.c's s_mutex around
 * s_registry, which IS how that file's registry access across tasks is
 * actually made safe -- not, as an earlier version of this comment wrongly
 * claimed, something that comes for free just because gap_event happens to
 * run on the same host task data_core_submit() is often called from. */
static batt_entry_t s_batt_tab[BATT_MAX_SENSORS];
static SemaphoreHandle_t s_batt_mutex;

/* battery_poll_start() and ble_collector_resume_scan() -- see
 * ble_collector_internal.h for both declarations. */

static int gap_event(struct ble_gap_event *event, void *arg);

/* Returns true iff passive scanning is actually running when this returns.
 * BLE_HS_EALREADY counts as running -- it means a scan was already up.
 *
 * M5a Task 6 fix round 1: this used to be void, and a failure was a log
 * line nobody could act on. It is not a theoretical failure: ble_gap_disc()
 * validates with ble_gap_conn_active() and returns BLE_HS_EBUSY while ANY
 * connect procedure is still outstanding, so a caller restarting the scan
 * right after an aborted connection can legitimately be refused. Nothing in
 * this firmware supervises scan health, so an unnoticed refusal means the
 * hub is deaf to every advertisement for the rest of the boot -- and
 * invisible with it, since adv_dropped cannot rise when nothing is being
 * received to drop. Reporting the outcome is what lets gatt_engine.c retry
 * (see gatt_engine_set_scan_resume()). */
static bool start_scan(void)
{
    struct ble_gap_disc_params params = {
        .passive = 1,
        .itvl = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS),
        .window = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS),
        .filter_duplicates = 0,   /* we want repeated frames; registry dedups */
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return false;
    }
    return true;
}

bool ble_collector_resume_scan(void)
{
    return start_scan();
}

/* M3 Task 3 (spec §4): BTHome v2 is a built-in decoder, matched by its
 * service UUID exactly like a wrapper's WMATCH_SERVICE key would be, but
 * dispatched here directly -- never through s_wrapper_index -- because it
 * lives outside the 16-wrapper store entirely and costs none of a user's
 * wrapper slots. Runs on adv_decoder_task, same as the rest of
 * decode_adv_item() below (flash/NVS reads -- bindkey_get() -- are only
 * safe off the NimBLE host task).
 *
 * gap_mac is the raw GAP address exactly as gap_event captured it
 * (ble_addr_t.val[], on-air wire order) -- NOT yet the human/display order
 * bthome_decode()'s mac[] contract requires (see bthome.h's doc comment on
 * why: NimBLE's val[0] is the first octet transmitted, the byte order's
 * OWN least-significant end, confirmed against nimble/ble.h's
 * BLE_ADDR_IS_RPA family keying off val[5] for the address-type bits).
 * Reversed once here into `mac`, which is then used consistently for both
 * the AES-CCM nonce (inside bthome_decode()) and this device's identity
 * (device_id_from_mac()) -- the same order data_core.c already uses
 * everywhere else (via mibeacon.c's own reversed m.mac), so a BTHome
 * device's dashboard/bind-key identity matches what a user would read off
 * the device or a scanner app. */
static void decode_bthome_item(const uint8_t *data, size_t len, const uint8_t gap_mac[6])
{
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = gap_mac[5 - i];

    device_id_t id = device_id_from_mac(DEV_KIND_BLE, mac);
    char dev_id[24];
    device_id_format(&id, dev_id, sizeof dev_id);

    uint8_t key_buf[16];
    bool have_key = bindkey_get(dev_id, key_buf);

    bthome_emit_t emits[BTHOME_MAX_EMITS];
    size_t n = 0;
    bthome_err_t err = bthome_decode(data, len, have_key ? key_buf : NULL, mac, emits, &n);
    if (err != BTHOME_OK) {
        /* Same low-noise convention as the MiFlora path just below (a
         * mibeacon_parse() failure there is likewise never logged above
         * DEBUG): a malformed/undecryptable frame from a real BTHome
         * device is expected background noise (a wrong bind key, a frame
         * clipped by the queue's 31-byte payload cap, ...), not something
         * that should spam the log on every advertisement interval. */
        ESP_LOGD(TAG, "bthome: %s decode failed (err=%d)", dev_id, (int)err);
        return;
    }

    /* M3 review fix 2: this device just decoded natively -- it no longer
     * belongs in the unknown-device capture (spec section 5: "a device that
     * later matches a wrapper is removed"; the same applies to a built-in
     * decoder match, which is exactly as final). unknown_capture_add()'s
     * only call site (decode_adv_item()'s no_match: label) is keyed by
     * item->mac, the raw GAP/on-air order -- gap_mac here IS that same
     * item->mac, unreversed, passed straight through from decode_adv_item()
     * (see this function's own top comment); `mac` just above is the
     * reversed display-order copy used for this device's identity/AES-CCM
     * nonce and must NOT be used here, or the forget would silently miss. */
    unknown_capture_forget(gap_mac);

    /* Each emit goes through data_core_submit_cap()'s own
     * capability_encode() + skip-on-CAP_VALUE_NONE discipline (M3 Task 3
     * brief: "the caller must SKIP the write in that case, never store the
     * sentinel") -- this function never calls capability_encode() itself. */
    bool wrote_any = false;
    for (size_t i = 0; i < n; i++) {
        if (data_core_submit_cap(mac, emits[i].cap_id, emits[i].value)) wrote_any = true;
    }
    /* Wake the rules engine only once real registry state changed --
     * mirrors "fires after a successful registry update, as the MiFlora
     * path does" (data_core_submit_from() -> rules_notify_value_update()
     * below), not the MiFlora path's own "every accepted frame regardless
     * of whether a value changed" looser trigger, since a BTHome frame with
     * every object out-of-range or unmapped (n==0, or every
     * data_core_submit_cap() skipped) wrote nothing for the engine to react
     * to. */
    if (wrote_any) rules_notify_value_update();
}

/* ---------------- M5b actuators: binding, dispatch, completion ---------- */

/* Ruling FINAL-persist: the actor table's opaque device key IS a
 * device_id_t, and note_actor_device() below hands one over by address.
 * Pinned here so a change to either definition is a build failure rather
 * than a guard file keyed on the wrong number of bytes. */
_Static_assert(sizeof(device_id_t) == ACTOR_DEVICE_KEY_LEN,
               "the actor table's device key is a device_id_t");

/* Reports a command that never reached the radio through the SAME path a
 * radio failure takes, so "every failure is visible" holds for the half of
 * the path that happens before the connection as well as after it. Declared
 * here and defined with the completion hook below. */
static void on_gatt_cmd_done(int dev_idx, bool ok, bool confirmed, const char *err);

/* Binds a matched device to the actor table the first time its wrapper is
 * seen to declare actions, and keeps its connection address fresh
 * afterwards. Runs on adv_decoder_task (the only task allowed to touch the
 * arena), once per advertisement of a matched device.
 *
 * The address refresh is not incidental: a BLE peripheral may use a
 * resolvable random address, and the one this hub can connect to is the one
 * it last advertised from. A command issued to an address the device has
 * since rotated away from simply never connects. */
static void note_actor_device(int idx, uint16_t wrapper_id, const adv_item_t *item)
{
    if (!s_actors_wired) return;
    if (idx < 0 || idx >= REGISTRY_MAX_DEVICES) return;

    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        if (s_actor_conn[i].dev_idx == (int8_t)idx) {
            s_actor_conn[i].addr_type = item->addr_type;
            memcpy(s_actor_conn[i].mac, item->mac, 6);
            s_actor_conn[i].wrapper_id = wrapper_id;
            return;
        }
    }

    if (s_actor_asked & (uint16_t)(1u << idx)) return;
    s_actor_asked |= (uint16_t)(1u << idx);

    wrapper_action_t acts[ACTOR_MAX_ACTIONS];
    uint8_t n = wrapper_exec_actions_list(wrapper_id, acts, ACTOR_MAX_ACTIONS);
    if (n == 0) return;   /* an ordinary sensor wrapper: nothing to bind */

    int slot = -1;
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        if (s_actor_conn[i].dev_idx < 0) { slot = i; break; }
    }
    if (slot < 0) {
        /* Same shape as registry_full_drops and actor_table_full_drops: a
         * fifth actuator is refused loudly, never silently ignored. Once
         * per device per reindex, because s_actor_asked is already set. */
        ESP_LOGW(TAG, "device %d declares actions but the hub already tracks %d actuators; "
                      "it cannot be commanded", idx, ACTOR_MAX_DEVICES);
        return;
    }

    s_actor_conn[slot].dev_idx = (int8_t)idx;
    s_actor_conn[slot].addr_type = item->addr_type;
    memcpy(s_actor_conn[slot].mac, item->mac, 6);
    s_actor_conn[slot].wrapper_id = wrapper_id;

    for (uint8_t i = 0; i < n; i++) {
        /* actor_declare() re-declares in place, leaving an operator's
         * guards and any hourly budget already spent untouched
         * (actor_table.h). param_max is the wrapper's own bound; the table
         * intersects it with the firmware's. */
        if (!actor_declare(idx, acts[i].action_id, acts[i].param_max, acts[i].flags)) {
            ESP_LOGW(TAG, "device %d: action %u could not be declared",
                     idx, (unsigned)acts[i].action_id);
        }
    }

    /* Whole-branch review, ruling FINAL-persist: this device's guards --
     * the operator's lockout, the cooldown, the hourly cap and how much of
     * that cap is already spent -- come back from flash HERE, in the same
     * call that declared the actions, keyed on the device's stable
     * identity rather than on its registry index (which the RAM-only
     * registry hands out in discovery order and so does not preserve
     * across a reboot).
     *
     * The placement is the point: actor_table_check() refuses any command
     * for a pair it does not know, so until the loop above ran nothing
     * could be commanded at all, and by the time this function returns the
     * guards are back. The only gap is a rules-task actor_request() landing
     * between the two, and actor_service_step()'s dispatch-time re-check
     * (Task 7 fix round 1) re-evaluates every guard before that command can
     * actually fire -- by which point this has completed, on this task.
     *
     * item->mac is raw GAP/on-air order and device_id_from_mac() takes
     * display order, so it is reversed here -- the same reversal
     * decode_adv_item() does for its own device_id_t. Recomputed rather
     * than passed in so this function stays callable from both of that
     * function's two dispatch paths without either having to remember to
     * hand it over. */
    uint8_t akey_mac[6];
    for (int i = 0; i < 6; i++) akey_mac[i] = item->mac[5 - i];
    device_id_t akey = device_id_from_mac(DEV_KIND_BLE, akey_mac);
    actor_set_device_key(idx, (const uint8_t *)&akey);
    actor_persist_restore_device(idx, (const uint8_t *)&akey);

    ESP_LOGI(TAG, "device %d bound as an actuator: %u action(s) from wrapper %u",
             idx, (unsigned)n, (unsigned)wrapper_id);
}

/* actor_service()'s dispatch hook: a command has passed every guard and the
 * radio is the next step. Runs on adv_decoder_task, which is where it must
 * run -- resolving the action entry reads the wrapper arena, and the arena
 * belongs to this task alone.
 *
 * Every early return reports through on_gatt_cmd_done(): this command has
 * already been popped from the actor queue and charged against its hourly
 * budget, so there is no second chance for it and no other place its
 * disappearance would surface. */
static void on_actor_dispatch(const actor_cmd_t *cmd)
{
    int idx = cmd->dev_idx;

    /* The whole command, retry bit included -- whether this one may be put
     * back is carried BY it, not tracked here (fix round 2). */
    s_cmd_inflight = *cmd;

    const actor_conn_t *conn = NULL;
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        if (s_actor_conn[i].dev_idx == (int8_t)idx) { conn = &s_actor_conn[i]; break; }
    }
    if (conn == NULL) {
        on_gatt_cmd_done(idx, false, false, "no connection address for this device yet");
        return;
    }

    uint16_t wrapper_id = (idx >= 0 && idx < REGISTRY_MAX_DEVICES)
                              ? s_wrapper_memo[idx] : WRAPPER_MEMO_NONE;
    if (wrapper_id == WRAPPER_MEMO_NONE) {
        on_gatt_cmd_done(idx, false, false, "no wrapper matched for this device");
        return;
    }

    /* Copied out of the arena here, on this task, for the same reason the
     * connect plan is (gatt_engine_request()): an arena pointer can be
     * invalidated by any later eviction, and these bytes are used much
     * later, on the NimBLE host task. */
    uint8_t entry[GATT_ACTION_ENTRY_MAX];
    uint16_t n = wrapper_exec_action_get(wrapper_id, cmd->action_id, entry, sizeof entry);
    if (n == 0) {
        on_gatt_cmd_done(idx, false, false, "the wrapper declares no such action");
        return;
    }

    /* Whole-branch review, Critical 1 + Important 2 (ruling FINAL-arm):
     * ARM THE OBLIGATION HERE, before the command is handed to the radio --
     * not from the completion hook, and not only on the branch where the
     * whole attempt succeeded. pending_close_arm_on_dispatch()'s own doc
     * comment in actor.h has the full reasoning; in short, the FSM writes
     * first and confirms second, so a failed require, a short confirm read
     * or a link drop in GS_READING all leave the valve OPEN while
     * reporting ok == false -- and the old arm ran only when ok was true.
     *
     * Flash is legal here: this function runs on adv_decoder_task, the only
     * task in this file allowed to touch LittleFS (the invariant the
     * s_pc_defer_* block above and gatt_engine.c both document). That is
     * also why the arm can be immediate rather than deferred a tick, which
     * is what closes Important 2's brownout window.
     *
     * Deliberately AFTER the three pre-radio refusals above: a command with
     * no connection address, no matched wrapper or no such action in the
     * wrapper cannot open anything, so it owes no close. Everything past
     * this point can. */
    uint8_t pc_flags = 0;
    if (actor_action_flags(idx, cmd->action_id, &pc_flags) &&
        pending_close_arm_on_dispatch((actor_source_t)cmd->source, cmd->action_id, pc_flags)) {
        pending_close_arm(idx, ACT_SWITCH_OFF, actor_now_s() + cmd->param);
    }

    gatt_engine_request_command(idx, entry, n, cmd->param, conn->mac, conn->addr_type);
}

/* gatt_engine_set_cmd_done_hook(): how a command ended. Runs on the NimBLE
 * host task for a radio outcome (and on adv_decoder_task for the pre-radio
 * refusals above), so it does only what is safe from both: alert_post() is
 * a fixed-size append under a critical section, explicitly safe from any
 * task including this one (alert.h), and the LittleFS/SSE half of the
 * report happens later, on the rules engine task, in alert_drain().
 *
 * A FAILED command alerts. An UNCONFIRMED one is logged but does not alert
 * from here: "completed unconfirmed" is a real outcome with its own policy
 * (spec section 4.4 -- an unconfirmed close counts as an open actuator and
 * is retried), and that policy belongs to the safety core, not to this
 * wiring. Alerting on it here as well would make the eventual retry read as
 * two failures. */
static void on_gatt_cmd_done(int dev_idx, bool ok, bool confirmed, const char *err)
{
    if (!ok) {
        /* The one retryable reason (gatt_engine.h): the radio belonged to
         * the battery poller at that instant. Nothing about the command or
         * the device was wrong, so it goes back on the queue rather than
         * being alerted and dropped -- once, and carrying its original
         * source and deadline so the TTL still bounds it and a safety close
         * keeps its priority. The actual actor_request() happens on
         * adv_decoder_task; see s_requeue's declaration. */
        if (err == GATT_CMD_ERR_RADIO_BUSY && !s_cmd_inflight.retried && !s_requeue_pending) {
            s_requeue = s_cmd_inflight;
            s_requeue_pending = true;   /* set LAST, like every cross-task flag here */
            ESP_LOGI(TAG, "command re-queued (radio was busy): dev=%d action=%u",
                     dev_idx, (unsigned)s_cmd_inflight.action_id);
            return;
        }
        ESP_LOGW(TAG, "command failed: dev=%d action=%u param=%u (%s)", dev_idx,
                 (unsigned)s_cmd_inflight.action_id, (unsigned)s_cmd_inflight.param,
                 err ? err : "unknown");
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_FAILED, dev_idx,
                   s_cmd_inflight.action_id, s_cmd_inflight.param);
        return;
    }
    ESP_LOGI(TAG, "command %s: dev=%d action=%u param=%u",
             confirmed ? "confirmed" : "completed UNCONFIRMED",
             dev_idx, (unsigned)s_cmd_inflight.action_id, (unsigned)s_cmd_inflight.param);

    /* M5b Task 9: the write landed. Only ONE thing follows now -- reporting
     * a CLOSE's outcome. The other half (arming an open's obligation) moved
     * to on_actor_dispatch(), BEFORE the radio, by the whole-branch
     * review's ruling FINAL-arm: arming from here could only ever happen on
     * the success branch, and the state machine writes before it confirms,
     * so every post-write failure left an open valve with no obligation.
     * See pending_close_arm_on_dispatch()'s doc comment in actor.h.
     *
     * This still only LATCHES what was learned; the flash-touching call
     * (pending_close_note_result()) happens on adv_decoder_task (see
     * s_pc_defer_kind's own comment: fix round 1, finding 1 -- this task
     * must never touch LittleFS). */
    if (s_cmd_inflight.source == ACTOR_SRC_SAFETY) {
        /* This WAS a scheduled (or retried) close: tell the safety core the
         * outcome so it can stop retrying once the close is actually
         * confirmed -- pending_close_note_result() itself treats "landed
         * but unconfirmed" (spec section 4.4) as still open and does
         * nothing, exactly like a genuine failure would. */
        s_pc_defer_dev_idx = dev_idx;
        s_pc_defer_ok = ok;
        s_pc_defer_confirmed = confirmed;
        s_pc_defer_kind = PC_DEFER_NOTE_RESULT; /* set LAST, like every cross-task flag here */
    }
}

/* The decoder-task half of the re-queue above. Called from the same loop
 * that pumps actor_service(), immediately BEFORE it, so a command put back
 * here can be popped in the very next pass rather than waiting a tick.
 *
 * actor_request() re-runs the guards, which is deliberate: an operator may
 * have hit lockout in the seconds since, and a command that was OK when it
 * was dispatched is not assumed still OK now -- the same argument Task 7's
 * dispatch-time re-check rests on. A refusal there posts its own named
 * alert, and this adds the "the command is gone" one on top, because those
 * are two different facts an operator needs. */
static void service_command_requeue(void)
{
    if (!s_requeue_pending) return;
    s_requeue_pending = false;

    actor_cmd_t cmd = s_requeue;

    /* actor_request_retry(), not actor_request(): same door, same guards,
     * but it stamps the command so this one can never be put back again --
     * for any number of commands in flight, since the bit rides in the
     * queue entry itself. */
    if (!actor_request_retry(cmd.dev_idx, cmd.action_id, cmd.param,
                             (actor_source_t)cmd.source, cmd.deadline_s)) {
        ESP_LOGW(TAG, "command could not be re-queued: dev=%d action=%u",
                 (int)cmd.dev_idx, (unsigned)cmd.action_id);
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_FAILED, cmd.dev_idx,
                   cmd.action_id, cmd.param);
    }
}

/* M5b Task 9 fix round 1 (finding 1): the decoder-task half of
 * on_gatt_cmd_done()'s pending-close deferral -- see s_pc_defer_kind's own
 * comment. This is where pending_close_note_result() actually runs, and so
 * where the LittleFS write it may trigger actually happens --
 * adv_decoder_task, same as every other flash access in this file. (The
 * arm half no longer passes through here at all: the whole-branch review's
 * ruling FINAL-arm moved it to on_actor_dispatch(), which already runs on
 * this task.)
 *
 * Fix round 3, finding 2 (comment correction -- the code was always
 * correct, the reasoning written next to it was not): a SECOND outcome
 * latching here while this function is mid-drain is not reachable, so the
 * clear-then-copy order below is not defending against one. Only one
 * command is ever in flight (s_cmd_inflight's own comment), so
 * on_gatt_cmd_done() fires at most once per dispatched command; and this
 * function is called (from adv_decoder_task's loop) strictly BEFORE
 * actor_service() dispatches the next one, so s_pc_defer_kind is always
 * PC_DEFER_NONE again by the time a new command -- and so a new possible
 * outcome -- could exist. (Clearing before copying would, if that
 * assumption were ever wrong, pair a STALE `kind` with a fresh payload,
 * which is the wrong direction to defend in anyway -- so this is not a
 * template to copy into code where the race is real.) */
static void service_pending_close_defer(void)
{
    if (s_pc_defer_kind == PC_DEFER_NONE) return;
    pc_defer_kind_t kind = s_pc_defer_kind;
    s_pc_defer_kind = PC_DEFER_NONE;

    int dev_idx = s_pc_defer_dev_idx;
    switch (kind) {
    case PC_DEFER_NOTE_RESULT: {
        bool ok = s_pc_defer_ok;
        bool confirmed = s_pc_defer_confirmed;
        pending_close_note_result(dev_idx, ok, confirmed);
        break;
    }
    case PC_DEFER_NONE:
    default:
        break;
    }
}

/* Runs on adv_decoder_task, never on the NimBLE host task -- this is where
 * flash reads / VM execution (Task 2 onward) are allowed to happen. For M3
 * Task 1 this is exactly the MiFlora decode that used to live inline in
 * gap_event, re-homed here unchanged (controller ruling on task-1-brief.md
 * Step 4: a build that stops reading its sensors is not an acceptable
 * intermediate state, even mid-milestone). Task 2 adds the match index and
 * dispatches to wrappers/BTHome on top of this. */
static void decode_adv_item(const adv_item_t *item)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, item->payload, item->len) != 0) return;

    /* M3 Task 2 matcher (spec §2): parse the advert exactly once (above) and
     * hand its service-UUID / manufacturer-id to the index, alongside the
     * item's own MAC for a mac_prefix match. wrapper_index_lookup()'s
     * contract is 0xFFFFFFFF for "this advert had none of that field" --
     * never 0, which is a real (if unlikely) UUID/company id. This runs for
     * every advert, MiFlora or not, since a non-MiFlora frame is exactly
     * what a wrapper exists to decode. */
    uint32_t svc_uuid = 0xFFFFFFFFu;
    if (fields.svc_data_uuid16 && fields.svc_data_uuid16_len >= 2) {
        svc_uuid = (uint32_t)(fields.svc_data_uuid16[0] | (fields.svc_data_uuid16[1] << 8));
    }
    uint32_t manu_id = 0xFFFFFFFFu;
    if (fields.mfg_data && fields.mfg_data_len >= 2) {
        manu_id = (uint32_t)(fields.mfg_data[0] | (fields.mfg_data[1] << 8));
    }

    /* M3 Task 3 (spec §4): BTHome dispatch happens BEFORE the wrapper index
     * is consulted -- it is a built-in decoder, held outside the
     * 16-wrapper store, never itself an entry in s_wrapper_index. A BTHome
     * advert still carries its 0xFCD2 service-data UUID like any other
     * service-keyed advert, so svc_uuid (computed just above) is reused
     * as-is rather than re-parsed. */
    if (svc_uuid == BTHOME_SVC_UUID) {
        decode_bthome_item(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, item->mac);
        return;
    }

    /* M3 Task 5 (spec §2 "device -> wrapper memo"): a memoised match skips
     * wrapper_index_lookup() entirely; a device the registry doesn't know
     * about yet (ridx < 0) or with no memo recorded falls through to a real
     * lookup, same as before this task. mac_disp is item->mac reversed into
     * display/human byte order -- device_id_from_mac()'s and
     * wrapper_exec_run()'s own contract, same reversal
     * decode_bthome_item() does independently for its own MAC use (see its
     * comment for why raw GAP order must never be used directly here).
     *
     * M3 review fix 3: wrapper_index_lookup()'s `mac` parameter is ALSO
     * display order (wrapper_index.h's top comment) -- a WMATCH_MAC_PREFIX
     * entry's key is packed from the human-typed/API-displayed prefix (e.g.
     * `match mac_prefix 0xD0CF13`, parsed verbatim by wrapper_store.c), not
     * the raw GAP order this file otherwise works in. Passing mac_disp here
     * (not item->mac) is what makes a mac_prefix wrapper actually fire on
     * the prefix its author typed. */
    uint8_t mac_disp[6];
    for (int i = 0; i < 6; i++) mac_disp[i] = item->mac[5 - i];
    device_id_t wid = device_id_from_mac(DEV_KIND_BLE, mac_disp);
    int ridx = data_core_find_index(&wid);
    int wrapper_id = (ridx >= 0 && s_wrapper_memo[ridx] != WRAPPER_MEMO_NONE)
                          ? s_wrapper_memo[ridx]
                          : wrapper_index_lookup(&s_wrapper_index, svc_uuid, manu_id, mac_disp);

    /* M5a gate fix (spec §2, amended 2026-08-17): the service-data
     * resolution above is right for M3's devices (BTHome, Xiaomi), which
     * broadcast their readings in service data (AD type 0x16), but a
     * connectable GATT peripheral -- the class of device M5a exists to
     * serve -- carries no service data at all. It advertises its services in
     * the 16-bit Service Class UUID list instead (AD types 0x02/0x03, which
     * NimBLE parses into fields.uuids16/num_uuids16), so the lookup above,
     * keyed only on service DATA, can never resolve one. Tried only when
     * that lookup (or the memo standing in for it) found nothing, so this
     * never overrides a real service-data/manufacturer/mac_prefix match.
     *
     * A wrapper's match key is registered ONCE, at install time, under a
     * single wmatch_kind_t (wrapper_store_upsert()) -- there is no separate
     * "advert" vs "connect" match kind, only WMATCH_SERVICE either way. So
     * this fallback is deliberately gated on wrapper_exec_plan_get() finding
     * a connect plan for the candidate id, NOT on how the id was found: that
     * is what stops it from ever matching an M3/M4 advert wrapper. An advert
     * wrapper carries no plan, so a UUID-list hit against one is rejected
     * here and the advert falls through to unknown-capture exactly as
     * before this fix -- no installed wrapper's behaviour changes. Same
     * plan-presence query devices_json.c already uses to tell the two kinds
     * apart on the API surface. Nothing below needs to remember that a
     * match came from THIS loop rather than the lookup above: every
     * candidate accepted here already carries a connect plan, and the
     * dispatch below (has_plan) now routes every plan-bearing wrapper away
     * from the advert-decode path regardless of how its id was found -- see
     * that dispatch's own comment. */
    if (wrapper_id < 0 && fields.uuids16 && fields.num_uuids16 > 0) {
        uint8_t n = fields.num_uuids16;
        if (n > ADV_UUID16_SCAN_MAX) n = ADV_UUID16_SCAN_MAX;
        for (uint8_t i = 0; i < n; i++) {
            int cand = wrapper_index_lookup(&s_wrapper_index, fields.uuids16[i].value,
                                            manu_id, mac_disp);
            if (cand >= 0 &&
                wrapper_exec_plan_get((uint16_t)cand, NULL, 0, NULL) > 0) {
                wrapper_id = cand;
                break;
            }
        }
    }

    if (wrapper_id >= 0) {
        /* M3 Task 6 (spec §5): this advert now resolves to a wrapper --
         * either it always did, or a wrapper install/reindex just made it
         * start matching. Either way it no longer belongs in the
         * unknown-device capture; no-op if it was never tracked there (see
         * unknown_capture_forget()'s own doc comment for why this call site,
         * not do_wrapper_reindex(), is the natural place). */
        unknown_capture_forget(item->mac);

        /* M5a gate fix round 2 (coordinator 2026-08-17): a matched wrapper
         * either declares a `connect` block or it doesn't, and that decides
         * EVERYTHING below -- resolved first, once, before any payload is
         * built. A connect wrapper's decode addresses named GATT buffers
         * that plainly don't exist on an advertisement: there has been no
         * GATT read yet. The advertisement's only job for such a wrapper is
         * "this device is awake, and may be due a read" -- the decode
         * belongs solely to gatt_engine_service(), against the concatenated
         * read buffer, once a read actually lands. Calling
         * wrapper_exec_run() here for a connect wrapper is exactly the bug
         * the hardware gate found: an empty/unrelated advert payload handed
         * to a decode whose first accessor addresses offset 0 of a buffer
         * that isn't there, so PSVM_ERR_REF ("bad reference") on every
         * single advertisement.
         *
         * Cheap: an arena cache hit in the common case, the same cost this
         * function's own UUID16-list fallback above already pays whenever
         * the service-data lookup misses. */
        uint32_t interval_s = 0;
        bool has_plan = wrapper_exec_plan_get((uint16_t)wrapper_id, NULL, 0, &interval_s) > 0;

        if (has_plan) {
            /* No wrapper_exec_run() on this path, on purpose (see above):
             * no payload slice is built, no decode runs, no last_error is
             * written from here, and match_count does not move -- for a
             * connect wrapper match_count now counts completed GATT reads
             * (wrapper_exec_run_buffer(), see wrapper_exec.h), the same "did
             * my wrapper actually run" signal wrapper_exec_run()'s bump
             * gives an advert wrapper. So a device that is matched but not
             * yet due a read does LITERALLY nothing else on this
             * advertisement below.
             *
             * M5a gate fix 3 (hardware gate, spec section 5/8): before this
             * fix, midx was just ridx, and a connect-only device deadlocked
             * here -- ridx is -1 on its very first advertisement (nothing
             * before this point can have registered it: no EMIT ever ran,
             * unlike the advert-wrapper branch below), so midx stayed -1
             * forever, so gatt_sched_due()/gatt_engine_request() below were
             * never reached, so no read was ever requested, so no EMIT ever
             * ran to create a registry entry -- no capability, no registry
             * entry, no GET /api/v1/devices row, forever, for the exact
             * class of device this milestone exists to serve.
             *
             * data_core_find_or_create_index() (data_core.h) gives this
             * advertisement itself a stable index when ridx doesn't already
             * have one -- the earliest possible point, with no capability
             * written yet (that function's own doc comment has the "every
             * consumer already tolerates zero capabilities" argument).
             * item->uptime_s (the same clock last_seen_s is measured in
             * everywhere else -- gatt_sched_ok()/fail(), the age_s
             * conversions in devices_json.c) stamps the new entry's
             * last_seen_s, since this advertisement genuinely IS the
             * sighting that justifies the slot existing.
             *
             * -1 here means the registry is already full and this MAC
             * isn't in it -- falls through exactly as -1 always has (no
             * memo write, no schedule check below), not fatal and not
             * silent: data_core_find_or_create_index() itself throttles and
             * warns (its own doc comment). Calling this on EVERY matched
             * advertisement of a still-unregistered device, rather than
             * trying once and giving up, is deliberate: it costs one
             * registry scan under the mutex ridx's own lookup above already
             * pays every advertisement, and it is the only way a device
             * that eventually gets a slot is ever picked up. */
            int midx = (ridx >= 0) ? ridx
                                   : data_core_find_or_create_index(&wid, item->uptime_s);
            if (midx >= 0 && midx < REGISTRY_MAX_DEVICES) {
                s_wrapper_memo[midx] = (uint16_t)wrapper_id;
                s_plan_interval_memo[midx] = interval_s;
                /* M5b: the same advertisement is also the only place a
                 * device's connection ADDRESS is observable, which is what
                 * a command needs and the registry does not hold. */
                note_actor_device(midx, (uint16_t)wrapper_id, item);
            }

            /* M3 Task 6 (spec sections 3 and 5): if this wrapper carries a
             * `connect` block, this advertisement is also the trigger for a
             * GATT read. Connecting on an advertisement is the ONLY
             * reliable moment for a battery sensor -- it is connectable for
             * a short window after it advertises and asleep the rest of the
             * time, so a periodic scheduler would spend most of its
             * attempts talking to a sleeping device.
             *
             * Nothing here connects: gatt_engine_request() sets a request
             * and returns, and the engine performs it on the NimBLE host
             * task. Same request/perform split as do_wrapper_reindex()
             * above, for the same reason -- a connection attempt is
             * hundreds of milliseconds of radio work and this task must
             * keep draining the advertisement ring. The plan/interval was
             * already resolved above, so gatt_sched_due() is the only gate
             * left. item->uptime_s is the uptime captured when gap_event
             * received this advertisement -- the same clock
             * gatt_sched_ok()/gatt_sched_fail() are stamped with. */
            if (midx >= 0 && !gatt_engine_busy() &&
                gatt_sched_due(midx, interval_s, item->uptime_s)) {
                /* item->mac/item->addr_type: the RAW GAP address, which is
                 * what gatt_engine_request() documents it wants (a
                 * connection is addressed on-air, not in display order) --
                 * NOT mac_disp. */
                gatt_engine_request((uint16_t)wrapper_id, midx, item->mac, item->addr_type);
            }
            return;
        }

        /* Below here: an advert wrapper (no connect plan), M3/M4's exact
         * semantics, untouched by M5a -- has_plan above is false for every
         * such wrapper (it never had a plan to find), so this is the same
         * code that always ran for it. */

        /* Payload slice convention mirrors decode_bthome_item()'s own
         * "hand the wrapper the bytes AFTER the matched AD structure's own
         * header, not the raw multi-structure advert" -- service-data past
         * its 2-byte UUID, manufacturer-data past its 2-byte company id.
         * mac_prefix has no such header to skip, so it gets the raw AD blob
         * as queued (item->payload/item->len). */
        const uint8_t *payload = NULL;
        uint8_t payload_len = 0;
        switch (wrapper_index_kind_of(&s_wrapper_index, (uint16_t)wrapper_id)) {
        case WMATCH_SERVICE:
            if (fields.svc_data_uuid16 && fields.svc_data_uuid16_len >= 2) {
                payload = fields.svc_data_uuid16 + 2;
                payload_len = (uint8_t)(fields.svc_data_uuid16_len - 2);
            }
            break;
        case WMATCH_MANUFACTURER:
            if (fields.mfg_data && fields.mfg_data_len >= 2) {
                payload = fields.mfg_data + 2;
                payload_len = (uint8_t)(fields.mfg_data_len - 2);
            }
            break;
        default:   /* WMATCH_MAC_PREFIX, or an id the memo outlived a reindex for */
            payload = item->payload;
            payload_len = item->len;
            break;
        }

        bool wrote = wrapper_exec_run((uint16_t)wrapper_id, mac_disp, payload, payload_len);

        /* Record the memo now that the device is guaranteed to be
         * registered (a successful EMIT just created/touched its registry
         * entry via data_core_submit_cap()) -- re-resolve rather than trust
         * `ridx` from above, which is -1 on this device's very first
         * advertisement. If the device STILL isn't registered (every EMIT
         * was skipped as out-of-range, or the wrapper's `require` rejected
         * this payload and emitted nothing), there is nothing to memoise
         * yet; the next advertisement just repeats a real lookup, which is
         * always correct, only not free. */
        int midx = (ridx >= 0) ? ridx : data_core_find_index(&wid);
        if (midx >= 0 && midx < REGISTRY_MAX_DEVICES) {
            s_wrapper_memo[midx] = (uint16_t)wrapper_id;
            /* Reached only for a wrapper with no connect block, so clear any
             * interval a previous connect wrapper left on this device --
             * otherwise editing a wrapper from connect to advert would leave
             * /api/v1/devices reporting a "gatt" object forever. */
            s_plan_interval_memo[midx] = 0;
            /* An actuator wrapper need not carry a connect plan at all --
             * an action table with no `connect` block is legal (psvm.h
             * gates the two sections on separate flags), so the binding
             * has to happen on this path too, not only on the plan one. */
            note_actor_device(midx, (uint16_t)wrapper_id, item);
        }
        if (wrote) rules_notify_value_update();
        return;   /* a matched wrapper's key can never also be MiFlora's (distinct match kinds/keys) -- nothing further to dispatch */
    }

    /* Native MiFlora path -- unchanged behaviour from before this task,
     * just reusing the single parse above instead of a second one. */
    if (!fields.svc_data_uuid16 || fields.svc_data_uuid16_len < 2 || svc_uuid != XIAOMI_SVC_UUID) goto no_match;
    mibeacon_t m;
    if (mibeacon_parse(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, &m) != MIBEACON_OK) goto no_match;
    if (m.product_id != MIBEACON_PRODUCT_MIFLORA) goto no_match;

    /* M3 review fix 2: same reasoning as decode_bthome_item()'s own forget
     * call above -- this device just decoded natively as a MiFlora, so it
     * no longer belongs in the unknown-device capture. item->mac is the
     * exact same raw GAP-order MAC this function's own no_match: label below
     * passes to unknown_capture_add(), so the two agree and this forget is
     * never a silent no-op for a device that IS tracked. */
    unknown_capture_forget(item->mac);

    /* Direct reception: no relaying node, just heard (age_s = 0). item->rssi
     * already has NimBLE's 127 "RSSI unavailable" sentinel (see ble_gap.h)
     * mapped to 0 by gap_event before this was queued -- a raw int8_t 127
     * would otherwise read as an implausibly strong signal (+127 dBm)
     * rather than "unknown". Since M5b, this rssi feeds
     * registry_update_from()'s "strongest rssi wins" attribution
     * (best_rssi), so passing 127 through unfiltered would let a reading
     * with genuinely no signal information ever recorded outrank every real
     * measurement, including a legitimate node relay. 0 is the same
     * "unknown" value data_core_submit()'s wrapper already uses for callers
     * with no RSSI at all. */
    data_core_submit_from(&m, NULL, item->rssi, 0);
    /* Wake the rules engine (spec section 4 "Triggers"): a plain
     * event-group bit set, safe to call from any task -- same "short,
     * bounded, never blocks" standard data_core_submit_from() itself is
     * held to just above. data_core_submit_from() is void (no
     * merged/duplicate/full signal to gate on), so this fires for every
     * accepted MiFlora frame, not just ones that changed a value; the
     * engine's own 2s debounce (rules_engine.c) absorbs that, and
     * re-resolving to the same reading on a duplicate value is harmless,
     * not a spurious fire (rules_fsm.h's edge/level semantics key off the
     * condition result, not off "did a value change"). Safe before
     * rules_init() has run (rules.h). */
    rules_notify_value_update();

    /* Direct reception (this file only ever handles the hub's own radio,
     * never a node relay): record it as a battery-poll candidate. item->mac
     * / item->addr_type are the GAP address captured verbatim in gap_event
     * -- ble_addr_t's byte order does not match m.mac's, so it must never
     * be reconstructed from the MiBeacon MAC. item->uptime_s is the
     * esp_timer uptime captured at the moment gap_event received this
     * advertisement, not at decode time, so a queue backlog can't skew
     * "last seen" into the future. s_batt_tab is shared with
     * battery_poll.c's poller task -- always locked. */
    xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
    batt_sched_seen(s_batt_tab, m.mac, item->addr_type, item->mac, item->uptime_s);
    xSemaphoreGive(s_batt_mutex);
    return;

no_match:
    /* M3 Task 6 (spec §5): every "nothing dispatched this" exit on this
     * path lands here -- BTHome didn't claim it (checked earlier, well
     * before this point), no wrapper's match key hit, and the native
     * MiFlora check just above also missed (wrong/missing service data, a
     * parse failure, or a non-MiFlora product id). item->mac/payload/len/
     * rssi/uptime_s are the queued item's own fields, unpacked here at the
     * call site rather than passed as an adv_item_t* (see unknown_capture.h's
     * top comment on why this module takes primitives instead). */
    unknown_capture_add(item->mac, item->payload, item->len, item->rssi, item->uptime_s);
}

/* See ble_collector.h's doc comment on ble_collector_wrapper_reindex_request()
 * (M3 Task 5, review FINDING 4): rebuild the match index from flash, then
 * throw away every cached bytecode blob and every device's memoised
 * wrapper id -- the three things spec §2 says a wrapper install/delete
 * must invalidate together, since any of those can change which wrapper id
 * (if any) a given device's advertisement should now resolve to. STATIC and
 * called ONLY from adv_decoder_task (below) -- never call this directly
 * from another task; see s_wrapper_reindex_pending's own comment for why. */
static void do_wrapper_reindex(void)
{
    wrapper_store_load_all(&s_wrapper_index);
    /* Bulk memset(), not a per-element atomic store -- see
     * ble_collector_wrapper_for_device()'s doc comment (ble_collector.h)
     * for what that means for a concurrent httpd read. */
    memset(s_wrapper_memo, WRAPPER_MEMO_NONE, sizeof(s_wrapper_memo));
    /* Cleared with the wrapper memo, not left to be refreshed. A device
     * whose connect wrapper was just DELETED never matches again, so it
     * never reaches either of the two writes below that would refresh this
     * -- both sit inside the "a wrapper matched" branch. Leaving the old
     * interval standing would keep /api/v1/devices reporting a "gatt"
     * object, with a stale interval and a stale last_read/last_error, for a
     * device that no longer has a plan at all. Zero is "no plan", which is
     * also the correct answer while nothing has matched yet. */
    memset(s_plan_interval_memo, 0, sizeof(s_plan_interval_memo));
    /* M5b: which actions a device supports comes from its wrapper, so an
     * install/edit/delete can change that answer too. The "already asked"
     * bitmap and the connection memo are cleared, so the next
     * advertisement from each device re-derives its binding from the new
     * index.
     *
     * A device whose wrapper is GONE would never re-derive anything, and
     * before fix round 1 it also stayed declared in the actor table
     * forever: every command a rule queued for it passed the guards,
     * reached dispatch, failed for want of a wrapper and alerted --
     * indefinitely, for a device the operator had removed. So each bound
     * device is asked ONE question here, while its previous wrapper id is
     * still known: does that wrapper still declare any action? If not, it
     * is undeclared outright.
     *
     * The question is asked per bound device (at most ACTOR_MAX_DEVICES,
     * so at most four arena loads on a reindex that already reloads the
     * whole index and evicts the arena) rather than by undeclaring
     * everything and re-declaring from scratch: actor_undeclare() takes
     * the device's guards, its spent hourly budget and its LOCKOUT with
     * it, and an operator must not lose a stop button because an
     * unrelated wrapper was installed. */
    s_actor_asked = 0;
    /* AFTER the eviction, never before: wrapper_exec_actions_list() below
     * loads through the arena, and a cached blob from the wrapper that was
     * just deleted would answer "still declares actions" for a wrapper that
     * no longer exists -- leaving the zombie this whole block removes. Past
     * the eviction the load goes to flash, where the delete is real. */
    wrapper_arena_evict_all();
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        if (s_actor_conn[i].dev_idx < 0) continue;
        wrapper_action_t acts[ACTOR_MAX_ACTIONS];
        if (wrapper_exec_actions_list(s_actor_conn[i].wrapper_id, acts, ACTOR_MAX_ACTIONS) == 0) {
            if (actor_undeclare(s_actor_conn[i].dev_idx)) {
                ESP_LOGI(TAG, "device %d undeclared: wrapper %u no longer declares actions",
                         (int)s_actor_conn[i].dev_idx, (unsigned)s_actor_conn[i].wrapper_id);
            }
        }
        s_actor_conn[i].dev_idx = -1;
    }
}

static void adv_decoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Checked every iteration -- both the "got an item" and "ring was
         * empty" paths below -- so a pending reindex request is picked up
         * promptly even during a burst of traffic, not just when the ring
         * happens to run dry. Always at the TOP of the loop, never between
         * popping an item and finishing decode_adv_item() for it, so a
         * reindex can never run mid-decode (s_wrapper_reindex_pending's own
         * comment; wrapper_arena.h's FINDING 2 doc comment on why that
         * matters). */
        if (s_wrapper_reindex_pending) {
            s_wrapper_reindex_pending = false;
            do_wrapper_reindex();
        }

        /* M5a Task 6: the decoder-task half of a GATT attempt -- the wrapper
         * decode of a completed read, and the event-log entry a finished
         * attempt owes. Both touch flash or run the VM, so neither may
         * happen on the NimBLE host task that drives the connection itself
         * (see gatt_engine.h's task-ownership note). Same safe point as the
         * reindex above and for the same reason: at the TOP of the loop,
         * never between popping an item and finishing decode_adv_item() for
         * it. A no-op when nothing is owed. */
        gatt_engine_service();

        /* M5b Task 8: the actor queue's pump. Here, on this task, because
         * the dispatch hook it calls resolves the wrapper's action entry
         * out of the arena -- flash, so decoder-task only, exactly like
         * gatt_engine_request() above it.
         *
         * The two predicates gating it are chosen precisely, because
         * popping a command is what CHARGES it: actor_service_step()
         * records the activation against the hourly budget and hands the
         * command on, and there is no un-charging it afterwards. So the
         * queue is pumped only when the command it pops can actually be
         * executed. Anything left unpopped keeps its TTL, its position and
         * a safety close's priority, and costs nothing but a tick.
         *
         *   gatt_engine_cmd_busy() -- OUR command is waiting for the radio
         *     or executing. The engine holds exactly one; a second popped
         *     on top of it would be refused on arrival. Cleared when that
         *     attempt ends.
         *
         *   battery_poll_busy() -- the OTHER owner of the hub's single
         *     outbound connection has it (fix round 2). Without this, a
         *     command popped during a poll was charged, refused by the
         *     engine, re-queued, and then refused AGAIN at the door by the
         *     cooldown its own dispatch had just started -- two alerts,
         *     both naming "cooldown" for what was really a busy radio, and
         *     deterministic for anyone who configured a cooldown at all.
         *     Not popping it is the whole fix: it dispatches a few hundred
         *     milliseconds later, uncharged and unrefused. This is the
         *     mirror image of the check battery_poll.c's handle_tick()
         *     already makes against gatt_engine_busy() before starting a
         *     poll, and it is bounded the same way -- a stuck poll is ended
         *     by that file's own POLL_WATCHDOG_S.
         *
         * gatt_engine_busy() is deliberately NOT among them. It is true
         * while a scheduled READ is in flight, and a command MUST be handed
         * to the engine during a read: that is what makes the engine hold
         * it, start it the instant the read ends, and refuse new reads
         * ahead of it (spec section 3's priority rule). Waiting for it
         * would leave the command in the queue where the next
         * advertisement could start another read in front of it. It is
         * also true because of our own pending command, which would make
         * this gate partly self-referential. */
        /* M5b Task 9: pending-close due-check, and the deferred
         * note_result drain that feeds it (fix round 1, finding 1) --
         * drained FIRST so a close just confirmed (PC_DEFER_NOTE_RESULT)
         * clears its obligation before this pass can re-attempt it. An
         * open's obligation needs no drain at all now: it is armed
         * synchronously in on_actor_dispatch(), on this same task, before
         * the command ever reaches the radio (ruling FINAL-arm), so it is
         * always already present by the time any due-check could see it.
         * Deliberately OUTSIDE the
         * s_actors_wired gate below and unconditional on role: unlike
         * actor_service(), pending_close_service() never touches the radio
         * itself -- it only decides whether an obligation is due and, if
         * so, calls actor_request() to enqueue the close, so it needs none
         * of the two busy gates that protect the pop-and-charge step, and
         * it must keep running even where s_actors_wired never becomes true
         * (a node, or the brief window on a hub before the GATT hooks are
         * registered) so that a stale obligation still retries, backs off
         * and -- on a node, which has no GATT radio to close anything with
         * at all -- eventually exhausts into a CRITICAL alert instead of
         * sitting in RAM forever, silently, because nothing ever called it
         * again. service_pending_close_defer() is a no-op check
         * (s_pc_defer_kind can only be set by the GATT completion hook,
         * which only fires once actors are wired) and pending_close_service()
         * is a no-op table scan (PENDING_CLOSE_MAX == 4 rows) when nothing
         * is due -- both cheap on every tick regardless of role. */
        service_pending_close_defer();
        pending_close_service();

        /* Ruling FINAL-persist: the guard file's only writer. A flag test
         * when nothing has changed, and the reason a guard change arriving
         * on the httpd task never touches flash from there. Outside the
         * s_actors_wired gate for the same reason pending_close_service()
         * is: it takes no radio, and a node that somehow holds guard state
         * should still be able to flush it. */
        actor_persist_service();

        if (s_actors_wired) {
            service_command_requeue();
            if (!gatt_engine_cmd_busy() && !battery_poll_busy()) actor_service();
        }

        adv_item_t item;
        portENTER_CRITICAL(&s_adv_mux);
        bool got = adv_ring_pop(&s_adv_ring, &item);
        portEXIT_CRITICAL(&s_adv_mux);
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(ADV_DECODER_POLL_MS));
            continue;
        }
        decode_adv_item(&item);
    }
}

/* Number of advertisements dropped because the ring was full when gap_event
 * tried to push -- exposed at GET /api/v1/status as "adv_dropped" (M3
 * spec §1: "so a saturated queue is visible rather than silent"). A single
 * uint32_t load is atomic on this target; no need for the critical section
 * push/pop take. */
uint32_t ble_collector_adv_dropped(void)
{
    return adv_ring_dropped(&s_adv_ring);
}

/* See ble_collector.h's doc comment. Safe to call from ANY task (Task 7's
 * future httpd handler is the intended caller) -- just sets a flag;
 * adv_decoder_task itself performs the actual reindex (do_wrapper_reindex()
 * above), at a safe point between decodes. Not yet called by anything in
 * M3 Task 5 (no install/delete API exists yet); wired here now so Task 7
 * only needs to call this one function. */
void ble_collector_wrapper_reindex_request(void)
{
    s_wrapper_reindex_pending = true;
}

/* See ble_collector.h's doc comment. */
int ble_collector_wrapper_for_device(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= REGISTRY_MAX_DEVICES) return -1;
    uint16_t id = s_wrapper_memo[dev_idx];
    return (id == WRAPPER_MEMO_NONE) ? -1 : (int)id;
}

/* See ble_collector.h's doc comment. */
uint32_t ble_collector_plan_interval_for_device(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= REGISTRY_MAX_DEVICES) return 0;
    return s_plan_interval_memo[dev_idx];
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* This callback runs on the NimBLE host task: it must never touch
         * flash, never block, and never run a VM (M3 spec §1). So it does
         * exactly one thing -- copy the raw advertisement into a queue item
         * -- and returns; adv_decoder_task (above) does all the actual
         * decoding off this task. */
        adv_item_t item;
        item.len = (event->disc.length_data > ADV_PAYLOAD_MAX)
                       ? ADV_PAYLOAD_MAX : (uint8_t)event->disc.length_data;
        memcpy(item.payload, event->disc.data, item.len);
        if (item.len < ADV_PAYLOAD_MAX) {
            memset(item.payload + item.len, 0, ADV_PAYLOAD_MAX - item.len);
        }
        /* Raw GAP address, NOT the MiBeacon's own MAC (that's only known
         * once payload is decoded, off this task) -- see decode_adv_item()'s
         * comment on why the two must never be conflated. */
        memcpy(item.mac, event->disc.addr.val, 6);
        item.addr_type = event->disc.addr.type;
        /* NimBLE uses 127 as its "RSSI unavailable" sentinel (ble_gap.h);
         * mapped to 0 here, before queueing, so every consumer of
         * adv_item_t.rssi already sees the same "unknown" value
         * data_core_submit()'s wrapper uses -- see decode_adv_item()'s
         * fuller comment on why this matters for best-rssi attribution. */
        item.rssi = (event->disc.rssi == 127) ? 0 : event->disc.rssi;
        item.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

        portENTER_CRITICAL(&s_adv_mux);
        adv_ring_push(&s_adv_ring, &item);   /* drop-and-count on full; never blocks */
        portEXIT_CRITICAL(&s_adv_mux);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        (void)start_scan();   /* should not happen with BLE_HS_FOREVER, but be safe */
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE synced, starting passive scan (itvl=%dms window=%dms)",
             CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS, CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS);
    (void)start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Same permanent boot-time heap trace main.c keeps (see its log_heap()),
 * one level further down. main.c's "after ble_collector_start" milestone
 * measures this whole function as one ~70 KB step; these two split it into
 * the parts that are actually tunable separately: nimble_port_init() brings
 * up the BLE CONTROLLER (BT_CTRL_BLE_MAX_ACT instances, scan duplicate
 * cache, adv-report flow-control buffers) plus the NimBLE HOST pools
 * (msys 1/2, transport ACL/event pools -- all CONFIG_BT_NIMBLE_* sized),
 * while nimble_port_freertos_init() adds only the host task's own stack
 * (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE). Round 4 cut the connection-side
 * pools and the controller instance count on the grounds that this product
 * is a passive OBSERVER with at most ONE outbound connection at a time
 * (battery_poll.c); these two lines are how the next boot log proves how
 * much that actually bought, and where the remainder still sits. */
static void log_heap(const char *milestone)
{
    ESP_LOGI(TAG, "heap @ %s: free=%u B largest_free_block(8BIT|INTERNAL)=%u B",
             milestone, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

esp_err_t ble_collector_start(void)
{
    log_heap("before nimble_port_init");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    log_heap("after nimble_port_init (controller + host pools)");

    /* Created before nimble_port_freertos_init() below starts the host
     * task -- gap_event (which locks this) must never be able to run
     * before the mutex exists. */
    s_batt_mutex = xSemaphoreCreateMutex();
    if (!s_batt_mutex) {
        ESP_LOGE(TAG, "battery table mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* Same ordering rule as s_batt_mutex above: the ring and its decoder
     * task must exist before nimble_port_freertos_init() below starts the
     * host task, since gap_event (which pushes to s_adv_ring) can start
     * running the instant that task exists. adv_ring_init() zeroes the
     * 712 B static ring (16 * 44 B items + bookkeeping -- M3 spec §7's
     * "16 * 48" budget); adv_decoder_task's own 3072 B stack is the only
     * heap cost this file's M3 pipeline adds, everything else above is
     * static. */
    adv_ring_init(&s_adv_ring);
    /* Also before the decoder task/host task can run: wrapper_store_load_all()
     * (M3 Task 2) does the boot's only wrapper-related LittleFS reads,
     * leaving s_wrapper_index fully built before adv_decoder_task's first
     * decode_adv_item() call can ever consult it. Static 192 B (16 * 12 B,
     * spec §7); the LittleFS scan itself is transient stack/heap, freed
     * before this returns. */
    wrapper_store_load_all(&s_wrapper_index);
    /* M3 Task 5: wire the arena's loader to the real flash reader before
     * anything can call wrapper_arena_get() (adv_decoder_task, once
     * created, below), then reset it to empty. Static
     * CONFIG_PLANTHUB_WRAPPER_ARENA bytes (2048 on esp32c3, 4096 on
     * esp32c5, spec §7). s_wrapper_memo (32 B static, M3 review fix 4 --
     * see its own declaration comment) starts all WRAPPER_MEMO_NONE --
     * memset(0xFF) is still correct at the new uint16_t width (every byte
     * of WRAPPER_MEMO_NONE=0xFFFFu is 0xFF, so filling byte-wise still
     * leaves each element 0xFFFF), not the default zero-init, since 0 is
     * itself a valid wrapper id. */
    wrapper_arena_set_loader(wrapper_store_read_psbc);
    wrapper_arena_init();
    memset(s_wrapper_memo, WRAPPER_MEMO_NONE, sizeof(s_wrapper_memo));
    /* M5b: the actuator table and command queue, and the connection memo
     * that binds a registry index to an address to connect to. Before
     * adv_decoder_task exists, because that task both fills the memo (from
     * every matched advertisement) and pumps the queue. dev_idx -1 marks a
     * free slot, so this cannot be left as the default zero-init: 0 is a
     * real registry index. */
    actor_init();
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) s_actor_conn[i].dev_idx = -1;
    /* Whole-branch review, ruling FINAL-persist: reads the guard file into
     * RAM. After actor_init() (which zeroes the table this restores INTO)
     * and before adv_decoder_task exists, so no device can be declared --
     * and so no restore attempted -- until the image is loaded. */
    actor_persist_init();
    /* M5b Task 9: pending-close persistence and boot replay. After
     * actor_init() (the table pending_close_service()'s actor_request()
     * calls check against must already be initialised, even though it is
     * still empty here -- no device is declared as an actuator until its
     * first advertisement is decoded, below). Reads the persisted file (if
     * any) and immediately attempts every surviving obligation; a device
     * not yet re-declared simply fails that attempt and is retried with
     * backoff (pending_close_service(), pumped from the decoder loop
     * below) until it is rediscovered or the retry budget is exhausted. */
    pending_close_init();
    /* M3 Task 6 (spec §5): reset the unknown-device capture to empty before
     * adv_decoder_task (below) can run decode_adv_item() and start filling
     * it. Static 768 B (see unknown_capture.c's top comment). */
    unknown_capture_init();
    log_heap("before adv_decoder_task");
    if (xTaskCreate(adv_decoder_task, "ble_adv_decoder", ADV_DECODER_TASK_STACK,
                     NULL, ADV_DECODER_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(adv_decoder_task) failed");
        return ESP_ERR_NO_MEM;
    }
    log_heap("after adv_decoder_task (decoder task stack)");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    log_heap("after nimble_port_freertos_init (host task stack)");

    /* Node role: battery polling connects out and would fight ESP-NOW
     * timing on the node's single radio. Scanning/relaying (above) still
     * runs on both roles; only the poll timer/task is hub-only. */
    if (swarm_store_role() != SWARM_ROLE_NODE) {
        battery_poll_start(s_batt_tab, s_batt_mutex);
        /* M5a Task 6: hub-only for exactly the reason battery polling above
         * is -- a GATT read connects out and would fight ESP-NOW timing on a
         * node's single radio (and spec section 9 makes GATT on nodes via
         * the swarm an M7 non-goal). Until this runs, gatt_engine_request()
         * is a no-op, so the decoder task above starting first is harmless:
         * requests from those first few advertisements are simply dropped,
         * exactly as they are whenever the engine is busy.
         *
         * After nimble_port_init() (top of this function): the engine's hops
         * onto the NimBLE host task allocate through the npl function table
         * that call installs. gatt_engine_set_scan_resume() injects the scan
         * restart rather than the engine calling into this file directly --
         * ble_collector already depends on gatt, and the reverse dependency
         * would be a cycle (same shape as wrapper_arena_set_loader()). */
        gatt_engine_init();
        gatt_engine_set_scan_resume(ble_collector_resume_scan);
        /* Fix round 1: the two owners of the hub's single outbound
         * connection made mutually aware. battery_poll.c's handle_tick()
         * checks gatt_engine_busy() in the other direction. Installed after
         * battery_poll_start() so the poller exists before the engine can
         * ask it anything. */
        gatt_engine_set_conn_busy_hook(battery_poll_busy);
        /* M5b Task 8: the two halves of the command path joined -- see this
         * file's "M5b actuators" block for why the join lives here and not
         * as a direct call in either component. Registered together, and
         * the completion hook FIRST: gatt_engine_request_command() refuses
         * outright when it has nowhere to report an outcome to, so a
         * command dispatched in the window between the two would be
         * dropped with only a log line. Hub-only for the same reason the
         * engine itself is (a node's radio belongs to ESP-NOW); on a node
         * the actor queue simply never gets pumped past its dispatch hook,
         * which stays NULL and is a documented no-op. */
        gatt_engine_set_cmd_done_hook(on_gatt_cmd_done);
        actor_set_dispatch_hook(on_actor_dispatch);
        s_actors_wired = true;   /* set LAST: it is what opens the path above */
    }
    return ESP_OK;
}
