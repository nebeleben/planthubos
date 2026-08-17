/* Runtime half of MiFlora battery polling. The pure scheduling decision
 * (battery_sched.c/.h) is host-tested; this file is the NimBLE-facing
 * driver ble_collector.c hands its s_batt_tab (+ the mutex guarding it) to
 * via battery_poll_start().
 *
 * Callback discipline (PlanV1 §8e, mirrored from ble_collector.c): the
 * esp_timer callback and every NimBLE host-task callback here
 * (poll_gap_event, read_cb) only ever queue a short fixed-size message to
 * poll_task -- they never block, connect, scan, or call data_core_submit*()
 * directly. All of that happens on poll_task instead.
 *
 * s_tab is shared with ble_collector.c's gap_event, running on the NimBLE
 * host task -- a different task from poll_task. Every access to s_tab here
 * holds s_batt_mutex for a short, bounded critical section (mirroring
 * data_core.c's s_mutex around s_registry); BLE calls themselves are always
 * made after releasing it, from a local copy of whatever table data they
 * need.
 */
#include "battery_sched.h"
#include "ble_collector_internal.h"
#include "gatt_engine.h"
#include "data_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "battery_poll";

#define POLL_TICK_S           60
#define POLL_CONN_TIMEOUT_MS  10000

/* Watchdog for a peer that accepts the connection but then never answers
 * the ATT read and never disconnects: without this, s_in_flight would stay
 * true forever and polling would silently stop for good. Bounded by the
 * poll_task wait loop below, not a separate timer. */
#define POLL_WATCHDOG_S        30

/* MiFlora battery/firmware characteristic 00001a02-0000-1000-8000-00805f9b34fb,
 * 128-bit little-endian byte array (BLE_UUID128_DECLARE order). */
#define MIFLORA_BATT_UUID \
    BLE_UUID128_DECLARE(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, \
                         0x00, 0x10, 0x00, 0x00, 0x02, 0x1a, 0x00, 0x00)

/* ble_collector_resume_scan() -- see ble_collector_internal.h. */

typedef enum {
    POLL_MSG_TICK,      /* 60s timer fired: consider starting a poll */
    POLL_MSG_RESULT,    /* read_cb got a valid battery reading */
    POLL_MSG_DONE,      /* the in-flight poll attempt is over (any outcome) */
} poll_msg_type_t;

typedef struct {
    poll_msg_type_t type;
    uint8_t mac[6];
    uint8_t battery_pct;
    uint32_t generation;   /* the poll this message belongs to -- see s_poll_generation below */
} poll_msg_t;

static batt_entry_t *s_tab;
static SemaphoreHandle_t s_batt_mutex;    /* guards every s_tab access -- shared with ble_collector.c */
static QueueHandle_t s_queue;

/* s_in_flight and s_inflight_started_s are WRITTEN only on poll_task: set
 * when a poll attempt starts (handle_tick) and cleared on POLL_MSG_DONE or
 * by the watchdog, both handled in poll_task's own loop -- so no lock is
 * needed for these (contrast s_tab above). Since M5a, s_in_flight is also
 * READ from the NimBLE host task, through battery_poll_busy(), by the GATT
 * engine deciding whether the radio is free; it is a single aligned bool
 * with one writer, so that read can see a stale value for a few
 * instructions but never a torn one, and the worst case is one GATT request
 * dropped or one poll deferred -- both of which the next advertisement or
 * the next 60 s tick retries anyway. An earlier version of this comment
 * claimed "no cross-task access", which stopped being true when that
 * accessor was added.
 *
 * s_conn_handle and s_got_result are NOT poll_task-only, despite an earlier
 * version of this comment claiming otherwise: s_conn_handle is written by
 * poll_gap_event and s_got_result by read_cb, and both of those run on the
 * NimBLE host task, a different task from poll_task. What actually makes
 * that safe is s_poll_generation below, not single-task ownership -- without
 * it, a read_cb/DONE that's still in flight when a watchdog abort ends a
 * poll (see handle_watchdog) could deliver its RESULT/DONE after poll_task
 * has already moved on to a new poll for a different sensor, misattributing
 * a battery value to the wrong s_tab entry or wrongly clearing s_in_flight
 * for that newer poll.
 *
 * s_polling_mac is written by poll_task (handle_tick) and only read by
 * read_cb on the host task to build its RESULT message -- safe without a
 * lock because ble_gap_connect() can't deliver a CONNECT event (and hence
 * read_cb can't run) for a new peer until handle_tick has already finished
 * writing s_polling_mac for that peer. */
static bool s_in_flight;
static uint8_t s_polling_mac[6];   /* which s_tab entry the in-flight poll is for */
static bool s_got_result;          /* read_cb already queued a value for this poll */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_inflight_started_s;

/* Bumped by handle_tick each time it starts a poll, then snapshotted into
 * the ble_gap_connect() cb_arg (poll_gap_event and, via that, read_cb both
 * receive it) and carried in every poll_msg_t. poll_task discards any
 * POLL_MSG_RESULT/POLL_MSG_DONE whose generation doesn't match the current
 * value -- see the comment above s_conn_handle for why that matters. */
static uint32_t s_poll_generation;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void post_simple(poll_msg_type_t type)
{
    poll_msg_t msg = { .type = type };
    xQueueSend(s_queue, &msg, 0);
}

/* Like post_simple(POLL_MSG_DONE), but stamped with the generation of the
 * poll this DONE belongs to, so a stale one (e.g. a disconnect callback
 * that fires after handle_watchdog has already aborted the poll and a new
 * one has since started) gets discarded by poll_task instead of clearing
 * s_in_flight for the wrong poll. */
static void post_done(uint32_t generation)
{
    poll_msg_t msg = { .type = POLL_MSG_DONE, .generation = generation };
    xQueueSend(s_queue, &msg, 0);
}

/* ble_gattc_read_by_uuid callback -- runs on the NimBLE host task. Fires
 * once per matching attribute (status 0) and once more with status
 * BLE_HS_EDONE when the procedure completes (attr NULL); MiFlora has a
 * single instance of this characteristic so at most one status-0 call is
 * expected. */
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    if (error->status == 0 && attr != NULL) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        uint8_t val = 0;
        if (len >= 1 && os_mbuf_copydata(attr->om, 0, 1, &val) == 0 && val <= 100) {
            s_got_result = true;
            poll_msg_t msg = { .type = POLL_MSG_RESULT, .battery_pct = val, .generation = generation };
            memcpy(msg.mac, s_polling_mac, 6);
            xQueueSend(s_queue, &msg, 0);
        } else {
            ESP_LOGI(TAG, "battery read: invalid payload (len=%u val=%u)", (unsigned)len, (unsigned)val);
        }
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (error->status == BLE_HS_EDONE) {
        if (!s_got_result) {
            ESP_LOGI(TAG, "battery read: characteristic not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        /* else: already terminated in the status==0 branch above. */
    } else {
        ESP_LOGI(TAG, "battery read failed: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int poll_gap_event(struct ble_gap_event *event, void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            /* Pass the same generation through: read_cb needs it to stamp
             * the POLL_MSG_RESULT it may queue. */
            int rc = ble_gattc_read_by_uuid(event->connect.conn_handle, 1, 0xffff,
                                             MIFLORA_BATT_UUID, read_cb, arg);
            if (rc != 0) {
                ESP_LOGI(TAG, "battery read_by_uuid failed to start: %d", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Connect failure or the 10s ble_gap_connect timeout land here
             * with the same nonzero status -- both just retry in an hour
             * (last_attempt_s is already set). */
            ESP_LOGI(TAG, "battery poll connect failed/timed out: %d", event->connect.status);
            post_done(generation);
            (void)ble_collector_resume_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        post_done(generation);
        (void)ble_collector_resume_scan();
        return 0;
    default:
        return 0;
    }
}

/* See ble_collector_internal.h. s_in_flight is written only by poll_task
 * (handle_tick/POLL_MSG_DONE/handle_watchdog) and read here from the NimBLE
 * host task; a single aligned bool, same "atomic enough on this target"
 * standard ble_collector.c's own volatile flags are held to. */
bool battery_poll_busy(void)
{
    return s_in_flight;
}

static void handle_tick(void)
{
    if (s_in_flight) return;   /* single in-flight poll */

    /* M5a Task 6 fix round 1: the hub has ONE outbound connection and, as
     * of M5a, two independent schedulers wanting it. Starting a poll while
     * a GATT read is in flight would fail this poll's connect AND, worse,
     * make its failure path call ble_collector_resume_scan() -- restarting
     * the scan while the GATT engine's link is still open. A poll is
     * retried in an hour anyway (and last_attempt_s is deliberately NOT
     * advanced here, so this defers rather than skips the poll). */
    if (gatt_engine_busy()) {
        ESP_LOGD(TAG, "skipping this poll tick: a GATT read has the radio");
        return;
    }

    uint32_t now = now_s();

    xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
    int idx = batt_sched_pick(s_tab, now);
    if (idx < 0) {
        xSemaphoreGive(s_batt_mutex);
        return;
    }
    s_tab[idx].last_attempt_s = now;
    memcpy(s_polling_mac, s_tab[idx].mac, 6);
    ble_addr_t peer = { .type = s_tab[idx].addr_type };
    memcpy(peer.val, s_tab[idx].addr_val, 6);
    xSemaphoreGive(s_batt_mutex);

    s_got_result = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_poll_generation++;
    uint32_t generation = s_poll_generation;

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGI(TAG, "disc_cancel before battery poll: %d (continuing anyway)", rc);
    }

    s_in_flight = true;
    s_inflight_started_s = now;
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, POLL_CONN_TIMEOUT_MS, NULL, poll_gap_event,
                         (void *)(uintptr_t)generation);
    if (rc != 0) {
        ESP_LOGI(TAG, "battery poll connect failed to start: %d", rc);
        s_in_flight = false;
        (void)ble_collector_resume_scan();
    }
}

static void handle_result(const poll_msg_t *msg)
{
    /* registry_set_battery() (via data_core_submit_battery()), not
     * data_core_submit()/registry_update_from() -- a synthetic
     * mibeacon_t{.frame_cnt=0} built here would risk being read as a
     * duplicate of whatever frame the sensor's last real advertisement
     * happened to carry frame_cnt 0, silently dropping this reading. See
     * registry_set_battery()'s doc comment. */
    bool applied = data_core_submit_battery(msg->mac, msg->battery_pct);
    if (!applied) {
        /* Registry full: this reading never landed anywhere, so don't
         * advance last_ok_s either -- otherwise the scheduler would wait a
         * full 24h to retry a poll that never actually succeeded. */
        ESP_LOGW(TAG, "battery reading for a polled sensor dropped (registry full)");
        return;
    }

    uint32_t now = now_s();
    xSemaphoreTake(s_batt_mutex, portMAX_DELAY);
    for (int i = 0; i < BATT_MAX_SENSORS; i++) {
        if (s_tab[i].in_use && memcmp(s_tab[i].mac, msg->mac, 6) == 0) {
            s_tab[i].last_ok_s = now;
            break;
        }
    }
    xSemaphoreGive(s_batt_mutex);
}

/* Aborts a stuck in-flight poll (peer connected but never answered the ATT
 * read and never disconnected on its own) so the scheduler isn't wedged
 * forever. Called only from poll_task's own wait-loop timeout, never from
 * a callback. */
static void handle_watchdog(void)
{
    if (!s_in_flight) return;
    if (now_s() - s_inflight_started_s <= POLL_WATCHDOG_S) return;

    ESP_LOGW(TAG, "battery poll watchdog: conn_handle=%u unresponsive after %us, aborting",
             (unsigned)s_conn_handle, (unsigned)POLL_WATCHDOG_S);
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_in_flight = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    (void)ble_collector_resume_scan();
    /* If the terminate above does still produce a DISCONNECT event, its
     * POLL_MSG_DONE/resume-scan are both idempotent no-ops on top of this. */
}

static void poll_task(void *arg)
{
    (void)arg;
    poll_msg_t msg;
    for (;;) {
        /* While a poll is in flight, wake at least once a second so the
         * watchdog above can notice a stuck peer within POLL_WATCHDOG_S;
         * otherwise block indefinitely for the next tick/result. */
        TickType_t wait = s_in_flight ? pdMS_TO_TICKS(1000) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &msg, wait) != pdTRUE) {
            handle_watchdog();
            continue;
        }
        switch (msg.type) {
        case POLL_MSG_TICK:
            handle_tick();
            break;
        case POLL_MSG_RESULT:
            /* Discard a RESULT left over from a poll poll_task has already
             * moved past (superseded by a new tick, or watchdog-aborted and
             * then superseded) -- s_polling_mac may by now belong to a
             * different sensor than the one read_cb captured it for. */
            if (msg.generation != s_poll_generation) break;
            handle_result(&msg);
            break;
        case POLL_MSG_DONE:
            /* Same staleness check: a DONE for an already-superseded poll
             * must not clear s_in_flight/s_conn_handle for the current one. */
            if (msg.generation != s_poll_generation) break;
            s_in_flight = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            break;
        }
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    post_simple(POLL_MSG_TICK);   /* never connect/scan/block from the timer callback */
}

void battery_poll_start(batt_entry_t *tab, SemaphoreHandle_t batt_mutex)
{
    s_tab = tab;
    s_batt_mutex = batt_mutex;
    s_queue = xQueueCreate(4, sizeof(poll_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed; battery polling disabled");
        return;
    }
    if (xTaskCreate(poll_task, "batt_poll", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed; battery polling disabled");
        return;
    }
    const esp_timer_create_args_t t = { .callback = timer_cb, .name = "batt_poll" };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&t, &timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_timer_start_periodic(timer, (uint64_t)POLL_TICK_S * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer start failed: %s", esp_err_to_name(err));
    }
}
