/* MQTT publisher -- Task 4 of the M6 integrations plan. Owns
 * integrations_start() (declared in integrations.h): reads the config once,
 * starts the esp-mqtt client itself when mqtt.enabled, then always calls
 * influx_start() (integr_private.h, Task 5's stub for now).
 *
 * Off by default: mqtt.enabled == false means this file never touches
 * esp-mqtt at all -- start_mqtt() is simply never called.
 *
 * Callback discipline (PlanV1 global constraint, restated here because it's
 * the one thing this file must never get wrong): data_event_handler() runs
 * on the default esp_event loop task. It must not publish or otherwise
 * block that task -- it copies the 6-byte sensor MAC into s_queue and
 * returns immediately. All MQTT publish traffic (state AND discovery)
 * happens exclusively on the dedicated mqtt_pub task below, which is the
 * only thing that ever calls esp_mqtt_client_publish(). */
#include "integrations.h"
#include "integr_config.h"
#include "integr_private.h"
#include "mqtt_json.h"
#include "data_core.h"
#include "registry.h"
#include "app_config.h"

#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_pub";

#define MQTT_PUB_QUEUE_DEPTH     8
#define MQTT_PUB_TASK_STACK      4096
#define MQTT_PUB_TASK_PRIO       3
/* Per-sensor state-publish throttle -- see the plan. A dropped/skipped
 * publish here is never lost data, just deferred to the sensor's next
 * advertisement cycle (well under 30s in practice). */
#define MQTT_PUB_MIN_INTERVAL_US ((int64_t)30 * 1000000)

#define TOPIC_BUF_SIZE      128
#define STATE_JSON_BUF_SIZE 256
#define DISC_JSON_BUF_SIZE  640

static const char *const METRICS[] = { "temp", "moisture", "lux", "conductivity", "battery" };
#define METRIC_COUNT (sizeof(METRICS) / sizeof(METRICS[0]))

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t            s_queue;      /* items: uint8_t mac[6] */
static char                     s_hub[16];

/* Per-registry-slot state, indexed by the same slot data_core_snapshot()'s
 * registry_t reports (registry_find() gives the index). Both arrays are
 * written only by mqtt_pub_task() / the MQTT_EVENT_CONNECTED handler, both
 * of which run on tasks the esp-mqtt/esp_event framework itself serialises
 * against this file's other entry point (data_event_handler(), which never
 * touches them) -- no separate mutex needed. */
static bool    s_discovery_sent[REGISTRY_MAX_SENSORS];
static int64_t s_last_pub_us[REGISTRY_MAX_SENSORS];

static void mac_to_hex12(const uint8_t mac[6], char out[13])
{
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Publishes all 5 HA discovery configs (retained, qos 1) for one sensor,
 * unconditionally -- not gated by latest.has_* (per the plan: a metric that
 * only starts appearing later would otherwise never get a discovery
 * config, and HA tolerates a config whose value template briefly yields
 * nothing). */
static void publish_discovery(const uint8_t mac[6])
{
    if (!s_client) return;

    char mac12[13];
    mac_to_hex12(mac, mac12);
    char name[33] = "";
    app_config_get_sensor_name(mac, name);

    char topic[TOPIC_BUF_SIZE];
    char json[DISC_JSON_BUF_SIZE];
    for (size_t i = 0; i < METRIC_COUNT; i++) {
        if (!mqtt_topic_discovery(topic, sizeof(topic), mac12, METRICS[i])) {
            ESP_LOGW(TAG, "discovery topic for %s/%s did not fit", mac12, METRICS[i]);
            continue;
        }
        if (!mqtt_json_discovery(json, sizeof(json), s_hub, mac12, name, METRICS[i])) {
            ESP_LOGW(TAG, "discovery payload for %s/%s did not fit", mac12, METRICS[i]);
            continue;
        }
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);
    }
}

/* Publishes the retained state payload for one registry entry, qos 0. */
static void publish_state(const sensor_entry_t *e)
{
    if (!s_client) return;

    char mac12[13];
    mac_to_hex12(e->mac, mac12);

    mqtt_state_t st = {
        .has_temp         = e->latest.has_temp,
        .has_moisture     = e->latest.has_moisture,
        .has_lux          = e->latest.has_lux,
        .has_conductivity = e->latest.has_conductivity,
        .has_battery      = e->latest.has_battery,
        .temp_c           = e->latest.temp_dc / 10.0f,
        .moisture_pct     = e->latest.moisture_pct,
        .lux              = e->latest.lux,
        .conductivity     = e->latest.conductivity_us,
        .battery_pct      = e->latest.battery_pct,
        .rssi             = e->best_rssi,
    };

    char topic[TOPIC_BUF_SIZE];
    char json[STATE_JSON_BUF_SIZE];
    if (!mqtt_topic_state(topic, sizeof(topic), s_hub, mac12)) {
        ESP_LOGW(TAG, "state topic for %s did not fit", mac12);
        return;
    }
    if (!mqtt_json_state(json, sizeof(json), &st)) {
        ESP_LOGW(TAG, "state payload for %s did not fit", mac12);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, json, 0, 0, 1);
}

/* The one task that ever calls esp_mqtt_client_publish(). Drains s_queue,
 * throttles to one state publish per sensor per 30s, and sends discovery
 * first for any sensor whose bitmap slot hasn't seen it this MQTT session.
 * registry_t (~1KB) lives on this task's own stack -- fine per the plan,
 * this is the one place in the component that does it. */
static void mqtt_pub_task(void *arg)
{
    (void)arg;
    uint8_t mac[6];
    registry_t snap;

    for (;;) {
        if (xQueueReceive(s_queue, mac, portMAX_DELAY) != pdTRUE) continue;

        data_core_snapshot(&snap);
        int idx = registry_find(&snap, mac);
        if (idx < 0) continue;   /* defensive: shouldn't happen, registry entries are never removed */

        int64_t now = esp_timer_get_time();
        if (now - s_last_pub_us[idx] < MQTT_PUB_MIN_INTERVAL_US) continue;
        s_last_pub_us[idx] = now;

        if (!s_discovery_sent[idx]) {
            publish_discovery(mac);
            s_discovery_sent[idx] = true;
        }
        publish_state(&snap.sensors[idx]);
    }
}

/* PLANTHUB_DATA_EVENT/DATA_EVENT_SENSOR_UPDATE handler -- default event
 * loop task. Callback discipline: queue-and-return only, see the file
 * header. Drop-oldest on a full queue (per the plan): the newest update
 * is kept rather than the stalest one already waiting. */
static void data_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_id;
    if (!s_queue || !event_data) return;

    if (xQueueSend(s_queue, event_data, 0) != pdTRUE) {
        uint8_t discard[6];
        (void)xQueueReceive(s_queue, discard, 0);
        (void)xQueueSend(s_queue, event_data, 0);
    }
}

/* Publishes discovery configs for every sensor currently in the registry
 * and marks their bitmap slots sent -- called right after CONNECTED, per
 * the plan, so mqtt_pub_task() doesn't redundantly resend them the first
 * time each sensor's next state publish comes through. */
static void publish_all_discovery(void)
{
    registry_t snap;
    data_core_snapshot(&snap);
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        if (!snap.sensors[i].in_use) continue;
        publish_discovery(snap.sensors[i].mac);
        s_discovery_sent[i] = true;
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");
        char topic[TOPIC_BUF_SIZE];
        if (mqtt_topic_avail(topic, sizeof(topic), s_hub)) {
            esp_mqtt_client_publish(event->client, topic, "online", 0, 1, 1);
        }
        /* Fresh MQTT session: reconnects included (per the plan -- esp-mqtt's
         * own auto-reconnect stays on; re-sending retained configs on every
         * reconnect is idempotent and cheap). */
        memset(s_discovery_sent, 0, sizeof(s_discovery_sent));
        publish_all_discovery();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker; esp-mqtt will auto-reconnect");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error event");
        break;
    default:
        break;
    }
}

static esp_err_t start_mqtt(const integr_config_t *cfg)
{
    app_config_hub_name(s_hub);

    s_queue = xQueueCreate(MQTT_PUB_QUEUE_DEPTH, 6);
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    char avail_topic[TOPIC_BUF_SIZE];
    if (!mqtt_topic_avail(avail_topic, sizeof(avail_topic), s_hub)) {
        ESP_LOGE(TAG, "availability topic did not fit for hub \"%s\"", s_hub);
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_mqtt_client_config_t mcfg = { 0 };
    mcfg.broker.address.uri = cfg->mqtt.uri;
    if (cfg->mqtt.user[0] != '\0') mcfg.credentials.username = cfg->mqtt.user;
    if (cfg->mqtt.pass[0] != '\0') mcfg.credentials.authentication.password = cfg->mqtt.pass;
    mcfg.session.last_will.topic  = avail_topic;
    mcfg.session.last_will.msg    = "offline";
    mcfg.session.last_will.qos    = 1;
    mcfg.session.last_will.retain = 1;

    /* esp_mqtt_client_init() duplicates every string/topic it needs
     * (esp_mqtt_set_config() -> esp_mqtt_set_if_config()/strdup() for the
     * LWT message internally) -- avail_topic and cfg's fields only need to
     * stay valid for this call, not for the client's lifetime. */
    s_client = esp_mqtt_client_init(&mcfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE, data_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(mqtt_pub_task, "mqtt_pub", MQTT_PUB_TASK_STACK, NULL, MQTT_PUB_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(mqtt_pub) failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mqtt starting: hub=%s broker=%s", s_hub, cfg->mqtt.uri);
    return ESP_OK;
}

esp_err_t integrations_start(void)
{
    integr_config_t cfg;
    integr_config_get(&cfg);

    if (cfg.mqtt.enabled) {
        esp_err_t err = start_mqtt(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mqtt failed to start (%s); influx unaffected", esp_err_to_name(err));
        }
    }

    return influx_start(&cfg);
}
