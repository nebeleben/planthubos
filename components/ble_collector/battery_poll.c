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

/* Implemented in ble_collector.c: resumes the passive scan a poll attempt
 * paused for its connect/read/terminate cycle. Not a public header -- this
 * poller is an internal implementation detail of ble_collector. */
extern void ble_collector_resume_scan(void);

typedef enum {
    POLL_MSG_TICK,      /* 60s timer fired: consider starting a poll */
    POLL_MSG_RESULT,    /* read_cb got a valid battery reading */
    POLL_MSG_DONE,      /* the in-flight poll attempt is over (any outcome) */
} poll_msg_type_t;

typedef struct {
    poll_msg_type_t type;
    uint8_t mac[6];
    uint8_t battery_pct;
} poll_msg_t;

static batt_entry_t *s_tab;
static SemaphoreHandle_t s_batt_mutex;    /* guards every s_tab access -- shared with ble_collector.c */
static QueueHandle_t s_queue;

/* All of the following are touched only on poll_task: set when a poll
 * attempt starts (handle_tick) and cleared on POLL_MSG_DONE or by the
 * watchdog, both handled in poll_task's own loop -- no cross-task access,
 * so no lock needed for these (contrast s_tab above). */
static bool s_in_flight;
static uint8_t s_polling_mac[6];   /* which s_tab entry the in-flight poll is for */
static bool s_got_result;          /* read_cb already queued a value for this poll */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_inflight_started_s;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void post_simple(poll_msg_type_t type)
{
    poll_msg_t msg = { .type = type };
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
    if (error->status == 0 && attr != NULL) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        uint8_t val = 0;
        if (len >= 1 && os_mbuf_copydata(attr->om, 0, 1, &val) == 0 && val <= 100) {
            s_got_result = true;
            poll_msg_t msg = { .type = POLL_MSG_RESULT, .battery_pct = val };
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
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            int rc = ble_gattc_read_by_uuid(event->connect.conn_handle, 1, 0xffff,
                                             MIFLORA_BATT_UUID, read_cb, NULL);
            if (rc != 0) {
                ESP_LOGI(TAG, "battery read_by_uuid failed to start: %d", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Connect failure or the 10s ble_gap_connect timeout land here
             * with the same nonzero status -- both just retry in an hour
             * (last_attempt_s is already set). */
            ESP_LOGI(TAG, "battery poll connect failed/timed out: %d", event->connect.status);
            post_simple(POLL_MSG_DONE);
            ble_collector_resume_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        post_simple(POLL_MSG_DONE);
        ble_collector_resume_scan();
        return 0;
    default:
        return 0;
    }
}

static void handle_tick(void)
{
    if (s_in_flight) return;   /* single in-flight poll */

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

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGI(TAG, "disc_cancel before battery poll: %d (continuing anyway)", rc);
    }

    s_in_flight = true;
    s_inflight_started_s = now;
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, POLL_CONN_TIMEOUT_MS, NULL, poll_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGI(TAG, "battery poll connect failed to start: %d", rc);
        s_in_flight = false;
        ble_collector_resume_scan();
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
    ble_collector_resume_scan();
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
        case POLL_MSG_TICK:   handle_tick(); break;
        case POLL_MSG_RESULT: handle_result(&msg); break;
        case POLL_MSG_DONE:   s_in_flight = false; s_conn_handle = BLE_HS_CONN_HANDLE_NONE; break;
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
