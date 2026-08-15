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
 * on the default esp_event loop task, and mqtt_event_handler() runs on
 * esp-mqtt's own task (esp-mqtt invokes its registered event handler inline
 * from its event loop, not from a separate dispatcher task) -- neither may
 * block or do any real work. data_event_handler() copies the 6-byte sensor
 * MAC into s_queue and returns immediately. mqtt_event_handler()'s
 * MQTT_EVENT_CONNECTED case does the same now: it publishes only the one
 * small retained "online" message directly (cheap: a single publish, no
 * NVS, no registry walk), clears the discovery bitmap, and posts a
 * RESYNC_DISCOVERY control message to s_queue rather than looping over the
 * registry and publishing discovery configs itself. That loop used to run
 * inline on esp-mqtt's task on every (re)connect -- up to 16 cold NVS name
 * reads plus up to 80 QoS-1 publishes, on a stack esp-mqtt sized for its
 * own needs, not this component's. All of that work (state publishes AND
 * discovery, whether triggered by a queued sensor update or by
 * RESYNC_DISCOVERY) now happens exclusively on the dedicated mqtt_pub task
 * below, which is the only thing that ever calls esp_mqtt_client_publish()
 * for state/discovery traffic, on its own stack sized for it (see
 * MQTT_PUB_TASK_STACK). */
#include "integrations.h"
#include "integr_config.h"
#include "integr_private.h"
#include "mqtt_json.h"
#include "data_core.h"
#include "registry.h"
#include "app_config.h"
#include "plants.h"
#include "plants_table.h"

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
/* 8192, not the plan's 4096 (M6 hardware round 1): moving the discovery
 * resync onto this task (the M1 final-review fix) put a ~1.2KB registry_t
 * snapshot plus the 640B discovery buffer, topic/name scratch and
 * esp-mqtt's publish path on this stack all at once, and the 4096 sizing
 * predates that move. On hardware the first CONNECTED -> resync overflowed
 * it instantly: a "Stack protection fault" panic in task mqtt_pub the
 * moment the broker accepted the connection, crash-looping the whole hub
 * (observed as the broker logging connect -> "connection closed by
 * client" every ~15s while the LWT flapped online/offline). */
#define MQTT_PUB_TASK_STACK      8192
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

/* s_queue items: a tagged union rather than a bare MAC, so the same queue
 * can carry both a per-sensor state update (from data_event_handler(), the
 * default esp_event loop task) and a RESYNC_DISCOVERY control message (from
 * mqtt_event_handler()'s MQTT_EVENT_CONNECTED case, esp-mqtt's task) --
 * both without either producer blocking or doing publish/NVS work itself.
 * mac is meaningful only for MQTT_PUB_MSG_STATE_UPDATE. */
/* MQTT_PUB_MSG_EVENT (Task 6, spec §5/§6): carries a rules/event_log
 * event's already-built JSON payload from mqtt_pub_event() (the event_log
 * hook's MQTT leg, wired in main.c) to mqtt_pub_task() for publish to
 * planthub/<hub>/event -- same single-publisher discipline as the two
 * message types above; only mqtt_pub_task() ever calls
 * esp_mqtt_client_publish(). */
typedef enum {
    MQTT_PUB_MSG_STATE_UPDATE,
    MQTT_PUB_MSG_RESYNC_DISCOVERY,
    MQTT_PUB_MSG_EVENT,
} mqtt_pub_msg_type_t;

typedef struct {
    mqtt_pub_msg_type_t type;
    uint8_t              mac[6];
    /* MQTT_PUB_MSG_EVENT only: heap copy made by mqtt_pub_event() (the
     * caller's own json buffer is not retained past that call); freed by
     * mqtt_pub_task() after publish, or immediately by mqtt_pub_event()
     * itself if the queue send fails. */
    char                 *event_json;
} mqtt_pub_msg_t;

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t            s_queue;      /* items: mqtt_pub_msg_t */
static char                     s_hub[16];

/* Per-plants-table-slot state, indexed by the same slot plants_snapshot()'s
 * plants_table_t reports (plants_table_find_id() gives the index) -- sized
 * PLANTS_MAX (16), not REGISTRY_MAX_SENSORS: a plant id is bounded (1..255,
 * never reused) but the table slot it currently occupies is what's bounded
 * *and* stable enough to index an array with, exactly like registry slots
 * were before this file went plant-centric. Unlike ids, slots CAN be reused
 * (plants_delete() frees a slot for a future plants_create()), so
 * s_slot_plant_id caches which plant id currently owns each slot: on a
 * mismatch (msg.mac resolved to a plant id that doesn't match what this
 * slot last published for), the slot has been reused by a different plant
 * since we last saw it, and its throttle/discovery state is stale and must
 * be reset before anything below trusts it.
 *
 * All four arrays are written only from mqtt_pub_task() -- discovery-sent
 * is set there when a discovery config actually goes out (whether
 * triggered by a queued state update or a RESYNC_DISCOVERY message), and
 * cleared by mqtt_event_handler()'s MQTT_EVENT_CONNECTED case on a
 * different task (esp-mqtt's), but only via memset() immediately before it
 * enqueues the RESYNC_DISCOVERY message that follows -- the queue
 * send/receive pair is a release/acquire, so mqtt_pub_task is guaranteed to
 * observe that memset before it next touches any of these arrays. No
 * separate mutex needed. */
static bool    s_discovery_sent[PLANTS_MAX];
static int64_t s_last_pub_us[PLANTS_MAX];
static uint8_t s_slot_plant_id[PLANTS_MAX];              /* 0 = never published from this slot */
static char    s_slot_name[PLANTS_MAX][PLANT_NAME_LEN + 1]; /* last name discovery was published with */

/* Publishes all 5 HA discovery configs (retained, qos 1) for one plant,
 * unconditionally -- not gated by latest.has_* (per the plan: a metric that
 * only starts appearing later would otherwise never get a discovery
 * config, and HA tolerates a config whose value template briefly yields
 * nothing). name is the plant's current display name or "" (mqtt_json.h's
 * mqtt_json_discovery() supplies the "Plant <id>" fallback). */
static void publish_discovery(uint8_t plant_id, const char *name)
{
    if (!s_client) return;

    char topic[TOPIC_BUF_SIZE];
    char json[DISC_JSON_BUF_SIZE];
    for (size_t i = 0; i < METRIC_COUNT; i++) {
        if (!mqtt_topic_discovery(topic, sizeof(topic), plant_id, METRICS[i])) {
            ESP_LOGW(TAG, "discovery topic for plant %u/%s did not fit", plant_id, METRICS[i]);
            continue;
        }
        if (!mqtt_json_discovery(json, sizeof(json), s_hub, plant_id, name, METRICS[i])) {
            ESP_LOGW(TAG, "discovery payload for plant %u/%s did not fit", plant_id, METRICS[i]);
            continue;
        }
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);
    }
}

/* Publishes the retained state payload for one plant, qos 0. e is the
 * registry entry for the plant's currently-assigned probe. */
static void publish_state(uint8_t plant_id, const sensor_entry_t *e)
{
    if (!s_client) return;

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
    if (!mqtt_topic_state(topic, sizeof(topic), s_hub, plant_id)) {
        ESP_LOGW(TAG, "state topic for plant %u did not fit", plant_id);
        return;
    }
    if (!mqtt_json_state(json, sizeof(json), &st)) {
        ESP_LOGW(TAG, "state payload for plant %u did not fit", plant_id);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, json, 0, 0, 1);
}

/* Publishes discovery configs for every plant currently in the plants table
 * (whether or not it has a probe assigned -- discovery just declares the HA
 * entity, and per-plant state publishing already only happens once that
 * plant has actually reported data) and marks their bitmap slots sent --
 * runs on mqtt_pub_task in response to a RESYNC_DISCOVERY message, so
 * mqtt_pub_task() itself doesn't redundantly resend them the first time
 * each plant's next state publish comes through. plants_table_t lives on
 * mqtt_pub_task's own stack here, same as registry_t/plants_table_t in
 * mqtt_pub_task() below -- both are fine on its stack (see
 * MQTT_PUB_TASK_STACK's comment for the sizing history). */
static void publish_all_discovery(void)
{
    plants_table_t snap;
    plants_snapshot(&snap);
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (!snap.p[i].in_use) continue;
        publish_discovery(snap.p[i].id, snap.p[i].name);
        s_discovery_sent[i] = true;
        s_slot_plant_id[i] = snap.p[i].id;
        strncpy(s_slot_name[i], snap.p[i].name, PLANT_NAME_LEN);
        s_slot_name[i][PLANT_NAME_LEN] = '\0';
    }
}

/* The one task that ever calls esp_mqtt_client_publish() for state/discovery
 * traffic. Drains s_queue: a STATE_UPDATE resolves the reporting mac to its
 * plant (plants_resolve_or_create() -- task context, allowed; this is one
 * of the sanctioned lazy-create call sites, since a DATA_EVENT mac is
 * always a real registry mac by construction), throttles to one state
 * publish per plant slot per 30s, and sends discovery first whenever that
 * slot hasn't seen it this MQTT session OR the plant's name has changed
 * since the cached one (a rename must re-publish discovery -- see
 * s_slot_name's comment); a RESYNC_DISCOVERY (posted by
 * mqtt_event_handler() right after MQTT_EVENT_CONNECTED, see the file
 * header) walks the whole plants table and (re)sends discovery for every
 * plant in it -- both the per-plant discovery publishes and this
 * full-table walk used to happen inline on esp-mqtt's own task; they
 * happen here now instead. registry_t/plants_table_t (~1KB each) live on
 * this task's own stack -- fine per the plan, this is the one place in the
 * component that does it. */
static void mqtt_pub_task(void *arg)
{
    (void)arg;
    mqtt_pub_msg_t msg;
    registry_t reg_snap;
    plants_table_t plants_snap;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.type == MQTT_PUB_MSG_RESYNC_DISCOVERY) {
            publish_all_discovery();
            continue;
        }

        if (msg.type == MQTT_PUB_MSG_EVENT) {
            if (s_client) {
                char topic[TOPIC_BUF_SIZE];
                int n = snprintf(topic, sizeof(topic), "planthub/%s/event", s_hub);
                if (n > 0 && (size_t)n < sizeof(topic)) {
                    esp_mqtt_client_publish(s_client, topic, msg.event_json, 0, 0, 0);
                } else {
                    ESP_LOGW(TAG, "event topic did not fit for hub \"%s\"", s_hub);
                }
            }
            free(msg.event_json);
            continue;
        }

        uint8_t plant_id = plants_resolve_or_create(msg.mac);
        if (plant_id == 0) continue;   /* plants table full; already logged once/mac/boot */

        data_core_snapshot(&reg_snap);
        int ridx = registry_find(&reg_snap, msg.mac);
        if (ridx < 0) continue;   /* defensive: shouldn't happen, registry entries are never removed */

        plants_snapshot(&plants_snap);
        int slot = plants_table_find_id(&plants_snap, plant_id);
        if (slot < 0) continue;   /* defensive: plants_resolve_or_create() just created/found it */

        if (s_slot_plant_id[slot] != plant_id) {
            /* Slot reused since we last saw it (or never seen) -- any
             * cached throttle/discovery state for it belongs to a
             * different plant and must not be trusted. */
            s_slot_plant_id[slot] = plant_id;
            s_last_pub_us[slot] = 0;
            s_discovery_sent[slot] = false;
            s_slot_name[slot][0] = '\0';
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_pub_us[slot] < MQTT_PUB_MIN_INTERVAL_US) continue;
        s_last_pub_us[slot] = now;

        const char *name = plants_snap.p[slot].name;
        bool name_changed = strncmp(s_slot_name[slot], name, PLANT_NAME_LEN) != 0;
        if (!s_discovery_sent[slot] || name_changed) {
            publish_discovery(plant_id, name);
            s_discovery_sent[slot] = true;
            strncpy(s_slot_name[slot], name, PLANT_NAME_LEN);
            s_slot_name[slot][PLANT_NAME_LEN] = '\0';
        }
        publish_state(plant_id, &reg_snap.sensors[ridx]);
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

    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_STATE_UPDATE };
    memcpy(msg.mac, event_data, sizeof(msg.mac));

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        mqtt_pub_msg_t discard;
        (void)xQueueReceive(s_queue, &discard, 0);
        (void)xQueueSend(s_queue, &msg, 0);
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
         * reconnect is idempotent and cheap). This handler runs on
         * esp-mqtt's own task (see the file header) and must not block or
         * do real work itself, so unlike the old shape it does not walk the
         * registry or publish discovery here -- it only clears the bitmap
         * and hands off to mqtt_pub_task via RESYNC_DISCOVERY. Non-blocking
         * send: if s_queue is momentarily full, this reconnect's resync is
         * simply skipped -- the *next* reconnect (or a first-seen sensor's
         * own state publish, which sends its discovery unconditionally) is
         * always another opportunity, so nothing here can justify blocking
         * esp-mqtt's task to guarantee delivery. */
        memset(s_discovery_sent, 0, sizeof(s_discovery_sent));
        if (s_queue) {
            mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_RESYNC_DISCOVERY };
            if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "mqtt_pub queue full; skipping this reconnect's discovery resync");
            }
        }
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

    s_queue = xQueueCreate(MQTT_PUB_QUEUE_DEPTH, sizeof(mqtt_pub_msg_t));
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

/* Enqueues a pre-built event JSON payload (main.c's event_log hook, spec
 * §5/§6) for publish to planthub/<hub>/event, QoS 0, non-retained --
 * declared directly in whichever caller needs it (main.c) rather than in a
 * header of its own, same as this file's own header comment explains for
 * why it, not some other file, owns every esp_mqtt_client_publish() call.
 * No-op when MQTT is disabled/not started (s_client/s_queue still NULL,
 * mirrors publish_discovery()/publish_state()'s own `if (!s_client)
 * return`) -- the caller doesn't need to check integr_config itself.
 * Best-effort: an event dropped here (disabled MQTT, or a momentarily full
 * queue) has already landed in the durable event_log ring and gone out over
 * SSE: MQTT is an integration leg, not the source of truth. */
void mqtt_pub_event(const char *json)
{
    if (!s_client || !s_queue || !json) return;

    char *copy = strdup(json);
    if (!copy) return;

    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_EVENT, .event_json = copy };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_pub queue full; dropping event publish");
        free(copy);
    }
}
