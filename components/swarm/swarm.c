/* Wires espnow_link + pairing + swarm_store to data_core: the hub ingests
 * node-forwarded readings through data_core_submit() (the same door the
 * local BLE collector uses), and a node forwards its own locally-heard
 * readings by subscribing to the existing PLANTHUB_DATA_EVENT. Neither
 * data_core nor ble_collector need any changes for this to work.
 */
#include "swarm.h"
#include "swarm_store.h"
#include "swarm_frame.h"
#include "espnow_link.h"
#include "pairing.h"
#include "data_core.h"
#include "registry.h"
#include "mibeacon.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "swarm";

/* ---------------- Hub side: ingestion + per-node RAM stats ---------------- */

typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t last_seen_s;
    uint32_t frames_rx;
    int8_t   rssi;
} node_stat_t;

static node_stat_t       s_stats[SWARM_MAX_NODES];
static SemaphoreHandle_t s_stats_mutex;
static uint32_t          s_frames_rx_total;

/* Called from the ESP-NOW receive callback (WiFi driver task): a short,
 * bounded critical section only (array scan over at most SWARM_MAX_NODES
 * entries, no I/O), same reasoning as data_core_submit()'s registry lock
 * below -- safe to take with portMAX_DELAY here because nothing that holds
 * s_stats_mutex ever blocks while holding it. */
static void record_stat(const uint8_t src[6], int rssi)
{
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);

    int idx = -1, free_idx = -1;
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (s_stats[i].in_use && memcmp(s_stats[i].mac, src, 6) == 0) { idx = i; break; }
        if (!s_stats[i].in_use && free_idx < 0) free_idx = i;
    }
    if (idx < 0) idx = free_idx;   /* -1 if the (tiny) table is somehow full */
    if (idx >= 0) {
        if (!s_stats[idx].in_use) {
            s_stats[idx].in_use = true;
            memcpy(s_stats[idx].mac, src, 6);
            s_stats[idx].frames_rx = 0;
        }
        s_stats[idx].last_seen_s = now_s;
        s_stats[idx].frames_rx++;
        s_stats[idx].rssi = (int8_t)rssi;
    }
    s_frames_rx_total++;

    xSemaphoreGive(s_stats_mutex);
}

/* A node-forwarded reading enters through exactly the same door as a
 * locally heard one, so storage, history, SSE and integrations need no
 * changes.
 *
 * data_core_submit() safety: verified against components/data_core/data_core.c.
 * It takes s_mutex with portMAX_DELAY, but the only things ever done under
 * that lock (here and in data_core_snapshot()) are registry_update()/memcpy
 * -- a short, bounded, allocation-free critical section with no further
 * blocking calls inside it -- so contention is bounded to microseconds, not
 * indefinite. The event it posts afterwards uses esp_event_post(..., 0),
 * i.e. it never blocks even if the event queue is full (the post is simply
 * dropped). So data_core_submit() is safe to call directly from the
 * ESP-NOW receive callback (WiFi driver task); no deferral to another task
 * is needed for this path. */
static void ingest_reading(const uint8_t src[6], const swarm_reading_t *r, int rssi)
{
    mibeacon_t m;
    memset(&m, 0, sizeof(m));
    memcpy(m.mac, r->mac, 6);
    m.product_id = MIBEACON_PRODUCT_MIFLORA;
    m.frame_cnt = r->frame_cnt;
    if (r->temp_dc != INT16_MIN)      { m.temp_dc = r->temp_dc; m.has_temp = true; }
    if (r->moisture_pct != 0xFF)      { m.moisture_pct = r->moisture_pct; m.has_moisture = true; }
    if (r->battery_pct != 0xFF)       { m.battery_pct = r->battery_pct; m.has_battery = true; }
    if (r->lux != 0xFFFFFFFFu)        { m.lux = r->lux; m.has_lux = true; }
    if (r->conductivity_us != 0xFFFF) { m.conductivity_us = r->conductivity_us; m.has_conductivity = true; }
    data_core_submit(&m);
    record_stat(src, rssi);
}

static void hub_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    int type = swarm_frame_type(data, (size_t)len);
    if (type == SWARM_MSG_READING) {
        swarm_reading_t r;
        if (swarm_decode_reading(data, (size_t)len, &r)) ingest_reading(src_mac, &r, rssi);
        return;
    }
    /* PAIR_REQ/PAIR_ACK, PING/PONG or anything unrecognised: pairing_handle_frame
     * already filters to PAIR_* and silently ignores everything else, so
     * handing it anything that isn't a reading is safe and keeps this
     * dispatcher a one-line decision. */
    pairing_handle_frame(src_mac, data, len, rssi);
}

esp_err_t swarm_start_main(void)
{
    if (!s_stats_mutex) s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) return ESP_ERR_NO_MEM;

    esp_err_t err = espnow_link_init(hub_rx_cb);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "swarm (main) started on channel %u", espnow_link_channel());
    return ESP_OK;
}

int swarm_node_list_json(char *buf, size_t cap)
{
    node_stat_t snap[SWARM_MAX_NODES];
    uint32_t total;

    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        memcpy(snap, s_stats, sizeof(snap));
        total = s_frames_rx_total;
        xSemaphoreGive(s_stats_mutex);
    } else {
        memset(snap, 0, sizeof(snap));
        total = 0;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < SWARM_MAX_NODES; i++) {
        if (!snap[i].in_use) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 snap[i].mac[0], snap[i].mac[1], snap[i].mac[2],
                 snap[i].mac[3], snap[i].mac[4], snap[i].mac[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", mac);
        cJSON_AddNumberToObject(o, "last_seen_s", snap[i].last_seen_s);
        cJSON_AddNumberToObject(o, "frames_rx", snap[i].frames_rx);
        cJSON_AddNumberToObject(o, "rssi", snap[i].rssi);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "frames_rx_total", total);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;
    int n = snprintf(buf, cap, "%s", body);
    free(body);
    return (n >= 0 && (size_t)n < cap) ? n : -1;
}

uint32_t swarm_frames_rx(void)
{
    if (!s_stats_mutex) return 0;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    uint32_t n = s_frames_rx_total;
    xSemaphoreGive(s_stats_mutex);
    return n;
}

/* ---------------- Node side: forwarding ---------------- */

#define SWARM_FWD_QUEUE_LEN     8
#define SWARM_FWD_FAIL_THRESHOLD 5

static QueueHandle_t s_fwd_queue;
static uint8_t       s_hub_mac[6];   /* set once in swarm_start_node(); MAC never
                                       * changes across a resync, only the channel does */

static void node_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    /* A node only ever expects PAIR_ACK here (initial pairing, handled by
     * pairing_node_start()'s own task); pairing_handle_frame() ignores
     * anything else. Readings are never received here, only sent. */
    pairing_handle_frame(src_mac, data, len, rssi);
}

/* data_core already posts this only when registry_update() reported NEW data
 * (a changed MiBeacon frame counter), so the node inherits the same dedup the
 * hub uses and transmits ~100x less than one frame per advertisement. Runs
 * on the default event-loop task: must not block, so it only builds the
 * frame and hands it to forward_task() via a queue -- the actual radio send
 * happens on that dedicated task instead. */
static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const uint8_t *mac = data;
    static registry_t snap;      /* event-loop task only */
    data_core_snapshot(&snap);
    int idx = registry_find(&snap, mac);
    if (idx < 0) return;
    const mibeacon_t *m = &snap.sensors[idx].latest;

    swarm_reading_t r = {
        .version = SWARM_PROTO_VERSION,
        .type = SWARM_MSG_READING,
        .frame_cnt = m->frame_cnt,
        .temp_dc = m->has_temp ? m->temp_dc : INT16_MIN,
        .moisture_pct = m->has_moisture ? m->moisture_pct : 0xFF,
        .battery_pct = m->has_battery ? m->battery_pct : 0xFF,
        .lux = m->has_lux ? m->lux : 0xFFFFFFFFu,
        .conductivity_us = m->has_conductivity ? m->conductivity_us : 0xFFFF,
        .rssi = 0,   /* BLE RSSI is not stashed anywhere in the collector/registry in M5a */
        .age_s = 0,  /* just heard */
        ._pad = 0,
    };
    memcpy(r.mac, mac, 6);

    if (!s_fwd_queue || xQueueSend(s_fwd_queue, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "forward queue full, dropping reading for " MACSTR, MAC2STR(mac));
    }
}

/* Owns every espnow_link_send() the node makes for readings, so a slow or
 * unreachable radio never stalls the default event-loop task (same
 * reasoning as sse.c's httpd_queue_work). On repeated failures, triggers a
 * channel resync rather than silently dropping forever. */
static void forward_task(void *arg)
{
    (void)arg;
    swarm_reading_t r;
    int consec_fail = 0;

    for (;;) {
        if (xQueueReceive(s_fwd_queue, &r, portMAX_DELAY) != pdTRUE) continue;

        uint8_t buf[sizeof(r)];
        size_t n = swarm_encode_reading(&r, buf, sizeof(buf));
        if (n == 0) continue;

        esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
        if (err == ESP_OK) {
            consec_fail = 0;
            continue;
        }

        consec_fail++;
        ESP_LOGW(TAG, "reading send failed (%s), consecutive=%d", esp_err_to_name(err), consec_fail);
        if (consec_fail >= SWARM_FWD_FAIL_THRESHOLD) {
            esp_err_t rerr = pairing_node_resync_channel();
            ESP_LOGI(TAG, "resync after %d consecutive failures: %s",
                     consec_fail, esp_err_to_name(rerr));
            consec_fail = 0;
        }
    }
}

esp_err_t swarm_start_node(void)
{
    /* The LMK isn't needed here: espnow_link_init() below re-adds the
     * stored hub peer (MAC + LMK + channel-follows-radio) on its own for a
     * SWARM_ROLE_NODE device, so this call only needs the MAC and channel. */
    uint8_t hub_mac[6], hub_ch;
    if (!swarm_store_hub(hub_mac, NULL, &hub_ch)) {
        ESP_LOGE(TAG, "swarm_start_node: no stored hub; caller must check swarm_store_hub() first");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_hub_mac, hub_mac, 6);

    /* Radio-only WiFi: ESP-NOW operates directly on the WiFi MAC layer and
     * needs no IP netif (the upstream esp-now example brings up WiFi the
     * same way), so unlike wifi_manager_start() this never creates a
     * STA/AP netif, never touches app_config's WiFi credentials and never
     * calls esp_wifi_connect() -- this device is simply never associated to
     * any AP, which is also what keeps espnow_link_set_channel() free to
     * hop channels below and during resync. */
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    err = espnow_link_init(node_rx_cb);
    if (err != ESP_OK) return err;

    err = espnow_link_set_channel(hub_ch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore channel %u: %s", hub_ch, esp_err_to_name(err));
    }

    s_fwd_queue = xQueueCreate(SWARM_FWD_QUEUE_LEN, sizeof(swarm_reading_t));
    if (!s_fwd_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(forward_task, "swarm_fwd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create forward task");
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE, on_sensor_update, NULL);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "node started: hub=" MACSTR " channel=%u", MAC2STR(hub_mac), hub_ch);
    return ESP_OK;
}

/* ---------------- Node side: searching for a hub (unpaired) ---------------- */

#define SWARM_PAIR_SEARCH_TIMEOUT_S 120

/* Polls pairing_node_state() rather than blocking on a semaphore signalled
 * by pairing_node_start()'s own task: that task's only externally-visible
 * outcome is this polled state (see pairing.h), so a small poll loop is
 * the simplest correct way to notice PAIR_OK/PAIR_FAILED and act on it. */
static void pair_watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        pairing_state_t st = pairing_node_state();
        if (st == PAIR_OK) {
            ESP_LOGI(TAG, "pairing succeeded; clearing pair-failed flag and rebooting as a paired node");
            swarm_store_set_pair_failed(false);
            esp_restart();
        } else if (st == PAIR_FAILED) {
            ESP_LOGW(TAG, "pairing failed/timed out; marking pair-failed and rebooting into the portal");
            swarm_store_set_pair_failed(true);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t swarm_start_node_search(void)
{
    /* Same radio-only bring-up as swarm_start_node(), but this device has
     * no stored hub yet -- it's actively looking for one, so there is no
     * channel to restore and node_rx_cb (which just forwards to
     * pairing_handle_frame) is exactly what's needed to receive PAIR_ACK. */
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    err = espnow_link_init(node_rx_cb);
    if (err != ESP_OK) return err;

    err = pairing_node_start(SWARM_PAIR_SEARCH_TIMEOUT_S);
    if (err != ESP_OK) return err;

    if (xTaskCreate(pair_watch_task, "swarm_pairwatch", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create pair-watch task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "searching for a hub (up to %ds)...", SWARM_PAIR_SEARCH_TIMEOUT_S);
    return ESP_OK;
}
