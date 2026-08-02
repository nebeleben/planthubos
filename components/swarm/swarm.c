/* Wires espnow_link + pairing + swarm_store to data_core: the hub ingests
 * node-forwarded readings through data_core_submit_from() (the same door
 * the local BLE collector uses, via data_core_submit()/data_core_submit_from()),
 * and a node forwards its own locally-heard readings by subscribing to the
 * existing PLANTHUB_DATA_EVENT. Neither data_core nor ble_collector need
 * any further changes for this to work.
 */
#include "swarm.h"
#include "swarm_store.h"
#include "swarm_frame.h"
#include "espnow_link.h"
#include "pairing.h"
#include "data_core.h"
#include "registry.h"
#include "mibeacon.h"
#include "app_config.h"

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
 * data_core_submit_from() safety: verified against
 * components/data_core/data_core.c. It takes s_mutex with portMAX_DELAY,
 * but the only things ever done under that lock (here and in
 * data_core_snapshot()) are registry_find()/registry_update_from()/memcpy
 * -- a short, bounded, allocation-free critical section with no further
 * blocking calls inside it -- so contention is bounded to microseconds, not
 * indefinite. The event it posts afterwards uses esp_event_post(..., 0),
 * i.e. it never blocks even if the event queue is full (the post is simply
 * dropped). So data_core_submit_from() is safe to call directly from the
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
    /* Attribute to the relaying node, using the node's own rssi/age_s from
     * the reading (not the ESP-NOW link rssi passed to this function,
     * which is the hub's signal to the node, not the node's signal to the
     * sensor). */
    data_core_submit_from(&m, src, r->rssi, r->age_s);
    record_stat(src, rssi);
}

/* True if mac is in swarm_store's persisted node table -- the same set
 * espnow_link_init() restores as ESP-NOW peers at boot. swarm_store's
 * table is a mutex-guarded RAM cache (see swarm_store.c), so this is a
 * short, bounded, allocation-free lookup over at most SWARM_MAX_NODES
 * entries: safe to call directly from the ESP-NOW receive callback, same
 * reasoning as every other swarm_store/s_stats_mutex access already made
 * from this path. No NVS/flash touched here. */
static bool is_paired_node(const uint8_t mac[6])
{
    int n = swarm_store_node_count();
    for (int i = 0; i < n; i++) {
        uint8_t stored[6];
        if (swarm_store_node_at(i, stored, NULL) && memcmp(stored, mac, 6) == 0) return true;
    }
    return false;
}

static void hub_rx_cb(const uint8_t src_mac[6], const uint8_t *data, int len, int rssi)
{
    int type = swarm_frame_type(data, (size_t)len);
    if (type == SWARM_MSG_READING) {
        /* PlanV1 3.3 promises "unpaired frames are dropped, so a neighbour
         * cannot inject readings" -- ESP-NOW hands this callback ANY
         * unencrypted frame from ANY MAC in range, paired or not, so that
         * promise has to be enforced here explicitly rather than assumed.
         * Without this check, any device broadcasting a well-formed
         * SWARM_MSG_READING could inject arbitrary (sensor MAC, values)
         * straight into data_core -> registry -> SSE/sampler/history,
         * no pairing window or claim key required. PAIR_REQ is
         * deliberately NOT gated this way -- that path is only ever live
         * during an operator-opened window and is pairing_handle_frame's
         * job to police. */
        if (!is_paired_node(src_mac)) {
            /* WARN (not DEBUG) and includes every stored node MAC being
             * compared against, not just the rejected sender -- this gate
             * started rejecting a legitimate, hardware-confirmed-paired
             * node immediately after 47f3db9 put the node's radio into
             * APSTA mode, and the leading theory is that ESP-NOW frames
             * from a node now egress tagged with its SoftAP interface MAC
             * (conventionally base-MAC + 1) rather than the STA MAC that
             * got stored at pairing time -- a mismatch that was invisible
             * before this gate existed, since nothing previously compared
             * the two. This log is diagnostic evidence for that, not a
             * fix: still rate-limited to once per 5s so a noisy/hostile
             * neighbour can't flood the console, and the gate's behaviour
             * is unchanged -- do not weaken or remove it without first
             * confirming what src_mac actually is here. */
            static int64_t s_last_drop_log_us;
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_drop_log_us > 5000000) {
                char known[SWARM_MAX_NODES * 20 + 8] = "";
                int n = swarm_store_node_count();
                for (int i = 0; i < n; i++) {
                    uint8_t mac[6];
                    if (!swarm_store_node_at(i, mac, NULL)) continue;
                    char one[20];
                    snprintf(one, sizeof(one), "%s" MACSTR, (known[0] != '\0') ? ", " : "", MAC2STR(mac));
                    strlcat(known, one, sizeof(known));
                }
                ESP_LOGW(TAG, "dropping READING from " MACSTR ", known nodes: %s",
                         MAC2STR(src_mac), n > 0 ? known : "(none)");
                s_last_drop_log_us = now_us;
            }
            return;
        }
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

    /* The definitive node list is swarm_store's PERSISTENT node table --
     * every adopted node lives there and survives a reboot. s_stats above
     * is only ever populated when a frame is actually received, so an
     * adopted node that hasn't transmitted (or hasn't been heard) yet
     * since this boot would previously be missing here entirely, even
     * though GET /api/v1/status already reported "paired":true for it --
     * a contradiction that made a one-sided-pairing defect look like a
     * hub-side bug when it wasn't. Merge in s_stats where available and
     * report null for last_seen_s/rssi (a real, meaningful "never heard"
     * value) and 0 for frames_rx (a real, meaningful count) otherwise. */
    int n_nodes = swarm_store_node_count();
    for (int i = 0; i < n_nodes; i++) {
        uint8_t mac[6];
        if (!swarm_store_node_at(i, mac, NULL)) continue;

        const node_stat_t *stat = NULL;
        for (int j = 0; j < SWARM_MAX_NODES; j++) {
            if (snap[j].in_use && memcmp(snap[j].mac, mac, 6) == 0) { stat = &snap[j]; break; }
        }

        char macstr[18];
        snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", macstr);
        if (stat) cJSON_AddNumberToObject(o, "last_seen_s", stat->last_seen_s);
        else cJSON_AddNullToObject(o, "last_seen_s");
        cJSON_AddNumberToObject(o, "frames_rx", stat ? stat->frames_rx : 0);
        if (stat) cJSON_AddNumberToObject(o, "rssi", stat->rssi);
        else cJSON_AddNullToObject(o, "rssi");
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
    /* One-shot: a working node->hub link is otherwise only inferable from
     * the hub side (frames_rx climbing in GET /api/v1/nodes) -- this makes
     * it positively visible on the node's own console the first time it
     * actually happens. */
    bool first_delivered = false;

    for (;;) {
        if (xQueueReceive(s_fwd_queue, &r, portMAX_DELAY) != pdTRUE) continue;

        uint8_t buf[sizeof(r)];
        size_t n = swarm_encode_reading(&r, buf, sizeof(buf));
        if (n == 0) continue;

        esp_err_t err = espnow_link_send(s_hub_mac, buf, n);
        if (err == ESP_OK) {
            consec_fail = 0;
            if (!first_delivered) {
                first_delivered = true;
                ESP_LOGI(TAG, "first reading delivered to hub");
            }
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

/* Brings up WiFi far enough for ESP-NOW without ever joining any network:
 * no esp_wifi_connect() call, ever -- this device is simply never
 * associated to any AP, which is what keeps espnow_link_set_channel()
 * free to hop channels for pairing/resync. No STA/AP netif either (ESP-NOW
 * operates directly on the WiFi MAC layer and needs no IP netif -- the
 * upstream esp-now example brings WiFi up the same way, and this held up
 * fine on real hardware for plain STA mode; nothing here requires a netif
 * for APSTA either since the softAP configured below is a decoy with no
 * IP-level function).
 *
 * WIFI_MODE_APSTA, not plain STA: confirmed on real hardware that a
 * unicast ESP-NOW frame from an AP-associated hub (the main hub is a
 * normal WiFi STA) gets silently filtered -- and never MAC-acked -- by an
 * unassociated node's radio. That's the well-known ESP-NOW + WiFi
 * mixed-mode coexistence trap, and Espressif's documented workaround for a
 * device that must exchange ESP-NOW frames with an AP-associated peer is
 * exactly this: bring the receiving side up in APSTA. (The handshake
 * itself was additionally fixed to use broadcast for PAIR_ACK, which
 * sidesteps this for that one frame regardless -- see pairing.c's
 * hub_task() -- but APSTA is also what should make the steady-state
 * node->hub unicast DATA frames reliable after pairing.) The softAP is a
 * throwaway: hidden (ssid_hidden) and configured for zero stations
 * (max_connection = 0), so it neither clutters the air nor accepts
 * anyone. Do NOT apply any of this to the hub -- its STA association must
 * keep working exactly as it does today; this function is node-only.
 *
 * WIFI_STORAGE_RAM + an explicit empty STA config matter for a reason that
 * only showed up on real hardware: wifi_manager's start_sta() (used when
 * this same device was previously a hub) calls esp_wifi_set_config() with
 * the driver's default WIFI_STORAGE_FLASH, which makes the WiFi driver
 * itself -- independently of app_config's own separate copy of the
 * credentials -- persist that STA config into its own flash-backed NVS
 * blob and auto-reload it on every esp_wifi_init(). Left alone, that meant
 * esp_wifi_start() here picked the old SSID/password back up and kept
 * trying to associate ("Haven't to connect to a suitable AP now!" every
 * ~300ms), fighting the pairing sweep's channel hops the whole time.
 * Switching to RAM storage and clearing the driver's live STA config (a
 * volatile, this-boot-only change) stops it -- app_config's own stored
 * credentials are never touched here, so they still work if this device
 * is later switched back to a main hub. */
static esp_err_t radio_only_wifi_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t empty_cfg = { 0 };
    err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
    if (err != ESP_OK) return err;

    char name[16];
    app_config_hub_name(name);
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, name, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(name);
    ap_cfg.ap.ssid_hidden = 1;
    ap_cfg.ap.max_connection = 0;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;

    return esp_wifi_start();
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

    esp_err_t err = radio_only_wifi_start();
    if (err != ESP_OK) return err;

    /* Order matters: espnow_link_init() below sets the regulatory domain
     * (esp_wifi_set_country_code()), which can move the radio's current
     * channel as a side effect (see the comment there). The stored hub
     * channel is restored AFTER espnow_link_init() returns, precisely so
     * that restore is the last word on which channel the radio ends up
     * on -- do not reorder these two calls. */
    err = espnow_link_init(node_rx_cb);
    if (err != ESP_OK) return err;

    err = espnow_link_set_channel(hub_ch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore channel %u: %s", hub_ch, esp_err_to_name(err));
    }
    /* Defensive re-assert: confirm the restore actually landed rather than
     * assuming it did. Should be a no-op given the ordering above, but
     * costs nothing to verify and self-correct if some other side effect
     * (country config or otherwise) ever moves the channel again. */
    uint8_t actual_ch = espnow_link_channel();
    if (actual_ch != hub_ch) {
        ESP_LOGW(TAG, "channel mismatch after restore: stored=%u actual=%u, retrying", hub_ch, actual_ch);
        espnow_link_set_channel(hub_ch);
        actual_ch = espnow_link_channel();
    }

    s_fwd_queue = xQueueCreate(SWARM_FWD_QUEUE_LEN, sizeof(swarm_reading_t));
    if (!s_fwd_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(forward_task, "swarm_fwd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create forward task");
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE, on_sensor_update, NULL);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "node started: hub=" MACSTR " stored_channel=%u actual_channel=%u",
             MAC2STR(hub_mac), hub_ch, actual_ch);
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
    esp_err_t err = radio_only_wifi_start();
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
