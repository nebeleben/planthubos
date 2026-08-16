/* MQTT publisher -- Task 4 of the M6 integrations plan, generalised onto
 * the capability table (registry v2/plants bindings) by M2 Task 7 (spec
 * Sec.6). Owns integrations_start() (declared in integrations.h): reads the
 * config once, starts the esp-mqtt client itself when mqtt.enabled, then
 * always calls influx_start() (integr_private.h, Task 5's stub for now).
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
 * own needs, not this component's. All of that work (plant AND device
 * state publishes, plant AND device-form discovery, and retained-topic
 * cleanup on plant delete/capability unbind/device removal/a device
 * capability newly becoming plant-bound -- Task 7) now happens exclusively
 * on the dedicated mqtt_pub task below, which is the only thing that ever
 * calls esp_mqtt_client_publish() for state/discovery/cleanup traffic, on
 * its own stack sized for it (see MQTT_PUB_TASK_STACK). api_v1.c's
 * plant-delete and capability-bind/unbind handlers, like every other
 * caller here, only ever ENQUEUE (mqtt_pub_plant_deleted()/
 * mqtt_pub_cap_unbound()/mqtt_pub_device_removed()/
 * mqtt_pub_device_cap_bound() below) -- never publish directly. */
#include "integrations.h"
#include "integr_config.h"
#include "integr_private.h"
#include "mqtt_json.h"
#include "data_core.h"
#include "registry.h"
#include "capability.h"
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
#include <stdlib.h>

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
 * client" every ~15s while the LWT flapped online/offline). M2 Task 7:
 * mqtt_pub_task()'s reg_snap is now a REAL registry_t (~2KB, up from the
 * ~832B M2 registry-compat shim it replaced) -- exactly the size this
 * stack was already budgeted for above, so 8192 stays unchanged. */
#define MQTT_PUB_TASK_STACK      8192
#define MQTT_PUB_TASK_PRIO       3
/* Per-sensor state-publish throttle -- see the plan. A dropped/skipped
 * publish here is never lost data, just deferred to the sensor's next
 * advertisement cycle (well under 30s in practice). */
#define MQTT_PUB_MIN_INTERVAL_US ((int64_t)30 * 1000000)

/* Discovery-burst pacing (M2 Task 7 hardware round 4): RESYNC_DISCOVERY can
 * fire up to PLANTS_MAX*CAPABILITY_COUNT + REGISTRY_MAX_DEVICES*
 * CAPABILITY_COUNT (16*8 + 16*8 = 256) individual esp_mqtt_client_publish()
 * calls back-to-back on reconnect -- hardware round 3 fixed the CONNECT
 * handshake, but the very next burst then exhausted esp-mqtt's outbox
 * ("outbox_enqueue: Memory exhausted") because every one of those was QoS 1
 * (V1's convention, confirmed by re-reading the pre-Task-7 file) and every
 * QoS>0 publish sits in the outbox until PUBACK. Dropping discovery/HA
 * publishes to QoS 0 (below) removes them from the outbox ENTIRELY --
 * esp_mqtt_client_publish() only calls the outbox-enqueueing path when
 * qos>0 (mqtt_client.c's mqtt_client_enqueue_publish()) -- which is the
 * real fix; this delay is the belt-and-braces second lever the review
 * asked for, keeping lwip's own send-side buffers shallow too even without
 * the outbox in the picture. 20 ms keeps a realistic burst (a handful of
 * plants/devices) imperceptible (tens of ms) while still bounding a
 * pathological one (256 messages -> ~5.1 s) to something that ends on its
 * own; see publish_discovery()/publish_device_discovery() for why this
 * cannot stall state publishes indefinitely -- only for the duration of
 * one (bounded, rare -- reconnect-only) burst, and mqtt_pub_task's own
 * queue absorbs what arrives meanwhile (bounded, drop-oldest, same as
 * every other momentary backlog this task already tolerates). */
#define MQTT_DISCOVERY_PACE_MS 20

#define TOPIC_BUF_SIZE      128
#define STATE_JSON_BUF_SIZE 320   /* up to CAPABILITY_COUNT fields keyed by full capability name (M2 Task 7, up from V1's 256) */
#define DISC_JSON_BUF_SIZE  1024  /* longer capability names + suggested_display_precision (M2 Task 7, up from V1's 640) */

/* esp-mqtt's OWN client task/buffers (esp_mqtt_client_config_t.task/.buffer
 * below, in start_mqtt()) -- NOT to be confused with MQTT_PUB_TASK_STACK
 * above, which sizes OUR mqtt_pub_task, a completely separate FreeRTOS
 * task esp-mqtt knows nothing about. M2 Task 7 hardware round 3
 * (task-7-report.md): IDF's defaults here (6144 B task stack, 1024 B
 * buffer shared for both directions since out_size falls back to size,
 * i.e. ~2048 B for the pair) are sized for a client that might do a TLS
 * handshake; ours never does (mqtt.uri is validated to always start with
 * "mqtt://" -- integr_config_set()) and never subscribes (no
 * MQTT_EVENT_DATA handler in this file), so both are real overshoot on a
 * C3 this starved for a single large contiguous free block.
 *
 *   - MQTT_CLIENT_TASK_STACK: no mbedtls call chain (the single biggest
 *     stack consumer TLS would add) and our own event callback
 *     (mqtt_event_handler() below) does only a stack-local 128 B snprintf,
 *     a switch, and an xQueueSend -- 4096 B is the conventional safe
 *     minimum Espressif's own non-TLS MQTT examples use; picked with
 *     margin, not shaved to the documented minimum, given this project's
 *     history of stack-sizing regressions (see MQTT_PUB_TASK_STACK's own
 *     comment) -- reported for hardware verification, not asserted as
 *     proven.
 *   - MQTT_CLIENT_IN_BUF_SIZE: this client only ever receives CONNACK/
 *     PUBACK/PINGRESP-class control packets (a few bytes each) -- 256 B
 *     leaves generous margin over that without keeping IDF's 1024 B
 *     receive-sized default for traffic that never arrives.
 *   - MQTT_CLIENT_OUT_BUF_SIZE: the largest PUBLISH this firmware can ever
 *     emit, measured (not guessed) by driving mqtt_json.c's
 *     mqtt_json_discovery()/mqtt_json_device_discovery() with worst-case
 *     inputs (max-length hub name, a 32-char all-`"`-character plant/
 *     device display name so every byte escapes to \", the longest
 *     canonical device id string, every capability in turn) plus its
 *     topic: the worst case is a device-form discovery publish at 81 B
 *     topic + 597 B payload = 678 B combined; +journal for the MQTT
 *     PUBLISH variable header (topic length prefix, QoS-1 packet id) and
 *     fixed header is at most ~10 B more. 1024 B (IDF's own default,
 *     unchanged -- confirmed correct by measurement, not assumed) leaves
 *     ~336 B of headroom over that measured worst case, comfortably
 *     covering any realistic display name (the all-quotes case is a
 *     pathological adversarial input, not a realistic one) plus MQTT
 *     framing overhead. */
#define MQTT_CLIENT_TASK_STACK   4096
#define MQTT_CLIENT_IN_BUF_SIZE   256
#define MQTT_CLIENT_OUT_BUF_SIZE 1024

/* s_queue items: a tagged union rather than a bare MAC, so the same queue
 * can carry a per-sensor state update (from data_event_handler(), the
 * default esp_event loop task), a RESYNC_DISCOVERY control message (from
 * mqtt_event_handler()'s MQTT_EVENT_CONNECTED case, esp-mqtt's task), an
 * event payload (mqtt_pub_event(), main.c's event_log hook) or a
 * retained-topic cleanup request (mqtt_pub_plant_deleted()/
 * mqtt_pub_cap_unbound()/mqtt_pub_device_removed()/
 * mqtt_pub_device_cap_bound(), api_v1.c -- M2 Task 7) -- all without any
 * producer blocking or doing publish/NVS work itself.
 * Only the field(s) documented for a message's own type are meaningful. */
typedef enum {
    MQTT_PUB_MSG_STATE_UPDATE,
    MQTT_PUB_MSG_RESYNC_DISCOVERY,
    MQTT_PUB_MSG_EVENT,
    MQTT_PUB_MSG_PLANT_DELETED,
    MQTT_PUB_MSG_CAP_UNBOUND,
    MQTT_PUB_MSG_DEVICE_REMOVED,
    MQTT_PUB_MSG_DEVICE_CAP_BOUND,
} mqtt_pub_msg_type_t;

typedef struct {
    mqtt_pub_msg_type_t type;
    uint8_t              mac[6];       /* STATE_UPDATE only */
    uint8_t               plant_id;     /* PLANT_DELETED, CAP_UNBOUND, DEVICE_CAP_BOUND (the plant
                                          * cap_id was just bound to -- H2 fixwave: drives the plant-
                                          * form discovery republish below, not just the device-form
                                          * cleanup) */
    uint8_t               cap_id;       /* CAP_UNBOUND, DEVICE_CAP_BOUND */
    device_id_t            dev;          /* DEVICE_REMOVED, DEVICE_CAP_BOUND */
    /* EVENT only: heap copy made by mqtt_pub_event() (the caller's own json
     * buffer is not retained past that call); freed by mqtt_pub_task()
     * after publish, or immediately by mqtt_pub_event() itself if the
     * queue send fails. */
    char                 *event_json;
} mqtt_pub_msg_t;

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t            s_queue;      /* items: mqtt_pub_msg_t */
static char                     s_hub[16];
/* Tracked from mqtt_event_handler()'s CONNECTED/DISCONNECTED cases (esp-mqtt
 * exposes no public "am I connected" query) -- M2 Task 7 hardware round 4:
 * gates the QoS-0 publish paths below (publish_state()/publish_device_
 * state()/publish_discovery()/publish_device_discovery()) so mqtt_pub_task
 * stops calling esp_mqtt_client_publish() into a dead client the moment a
 * disconnect is known, instead of discovering it per-call via esp-mqtt's own
 * "Losing qos0 data when client not connected" (a QoS-0 publish while
 * disconnected is unconditionally dropped -- no outbox, no retry -- so the
 * call was always pure waste once disconnected; see esp_mqtt_client_publish()
 * in mqtt_client.c). Deliberately NOT consulted by cleanup_plant()/
 * cleanup_cap()/cleanup_device_cap_discovery() below -- those stay QoS 1,
 * where a publish while disconnected is legitimately ENQUEUED in esp-mqtt's
 * outbox for delivery on the next reconnect, not wasted. `volatile`: written
 * on esp-mqtt's own task (mqtt_event_handler), read on mqtt_pub_task -- a
 * plain flag read/write race is fine here (worst case one extra/skipped
 * publish attempt around the exact moment of a state transition), so this
 * doesn't need a mutex, just the compiler not caching a stale value across
 * the loop iterations that read it. */
static volatile bool s_mqtt_connected;

/* Per-plants-table-slot state, indexed by the same slot plants_snapshot()'s
 * plants_table_t reports (plants_table_find_id() gives the index) -- sized
 * PLANTS_MAX (16), not REGISTRY_MAX_DEVICES: a plant id is bounded (1..255,
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

/* Device-form discovery "sent" bitmap (spec Sec.6, amended -- dedup by
 * device id + capability id), indexed by the device's REGISTRY slot
 * (registry_find()'s return value), NOT by a plant slot: unlike plants
 * table slots, a registry slot is stable for a device's whole lifetime
 * once created (registry.c never evicts an entry -- see plants.h's
 * plants_adopt_from_registry() doc comment), so this needs no
 * generation/reuse guard the way s_slot_plant_id above does. Cleared
 * whole-table on reconnect, same as s_discovery_sent.
 *
 * HEAP, not static (M2 Task 7 hardware round -- see task-7-report.md's
 * heap-reclaim section, same rationale as influx.c's s_batch): 128 B is
 * small next to that file's 8.6 KB, but it is still 128 B every hub's
 * .bss paid regardless of mqtt.enabled, and every byte mattered on the C3
 * repro. calloc'd (zero-initialised, matching the old static's implicit
 * zero-init) in start_mqtt() only when MQTT is actually starting; NULL
 * otherwise, which is safe because every reader of it
 * (publish_device_discovery(), and the memset on reconnect) only runs
 * once start_mqtt() has already succeeded. */
static bool (*s_device_disc_sent)[CAPABILITY_COUNT];

/* Publishes one HA discovery config (retained, **qos 0** -- see below) per
 * capability CURRENTLY bound on plant_id (plants_bindings()) -- unlike V1's
 * fixed 5-metric loop, a plant may have any subset of CAPABILITY_COUNT
 * bound, and this only publishes for those, per the spec's "one entity per
 * bound capability". Unconditional per binding (not gated by whether the
 * capability has a live value yet): a metric that only starts appearing
 * later would otherwise never get a discovery config, and HA tolerates a
 * config whose value template briefly yields nothing. name is the plant's
 * current display name or "" (mqtt_json.h's mqtt_json_discovery() supplies
 * the "Plant <id>" fallback).
 *
 * QoS 0, not V1's QoS 1 (hardware round 4, task-7-report.md): retained QoS 0
 * is legitimate for HA discovery -- the broker keeps the config regardless
 * of QoS, and this file already re-publishes the WHOLE discovery set on
 * every reconnect (RESYNC_DISCOVERY) and whenever a plant's name changes,
 * so QoS 1's "guaranteed, deduplicated, acked delivery" buys little here
 * that isn't already covered by the periodic resync -- while its cost (every
 * message sits in esp-mqtt's outbox until PUBACK) is exactly what exhausted
 * the outbox during a reconnect's discovery burst. Gated on s_mqtt_connected
 * (not just s_client) since a QoS-0 publish while disconnected is otherwise
 * silently dropped for nothing -- see that flag's own comment -- and
 * re-checked every iteration so a disconnect mid-burst stops the rest of it
 * immediately rather than working through however many capabilities remain.
 * Paced (MQTT_DISCOVERY_PACE_MS) between messages -- see that constant's
 * comment for the full reasoning; this keeps mqtt_pub_task inside this call
 * for at most tens of ms in the realistic case. */
static void publish_discovery(uint8_t plant_id, const char *name)
{
    if (!s_client) return;

    plant_binding_t bindings[CAPABILITY_COUNT];
    size_t n = plants_bindings(plant_id, bindings, CAPABILITY_COUNT);

    char topic[TOPIC_BUF_SIZE];
    char json[DISC_JSON_BUF_SIZE];
    for (size_t i = 0; i < n; i++) {
        if (!s_mqtt_connected) break;   /* disconnected mid-burst: stop, don't waste the rest */
        uint8_t cap_id = bindings[i].cap_id;
        if (!mqtt_topic_discovery(topic, sizeof(topic), plant_id, cap_id)) {
            ESP_LOGW(TAG, "discovery topic for plant %u/cap %u did not fit", plant_id, cap_id);
            continue;
        }
        if (!mqtt_json_discovery(json, sizeof(json), s_hub, plant_id, name, cap_id)) {
            ESP_LOGW(TAG, "discovery payload for plant %u/cap %u did not fit", plant_id, cap_id);
            continue;
        }
        esp_mqtt_client_publish(s_client, topic, json, 0, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_PACE_MS));
    }
}

/* Publishes the retained state payload for one plant, qos 0: one field per
 * CURRENTLY bound capability with a live (non-stale-checked -- this is the
 * live event-triggered path, not influx.c's periodic stale-filtered sweep)
 * value, read straight off `reg` via plants_cap_value(). A plant with no
 * bound capability yet reporting a value publishes nothing (mirrors V1's
 * "nothing to say yet" case). */
static void publish_state(uint8_t plant_id, const registry_t *reg)
{
    if (!s_client || !s_mqtt_connected) return;   /* QoS 0: a publish while disconnected is pure waste -- see s_mqtt_connected's comment */

    plant_binding_t bindings[CAPABILITY_COUNT];
    size_t n = plants_bindings(plant_id, bindings, CAPABILITY_COUNT);

    mqtt_state_t st = { 0 };
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t cap_id = bindings[i].cap_id;
        float value;
        uint32_t age_s;
        if (!plants_cap_value(plant_id, cap_id, reg, &value, &age_s)) continue;
        st.present[cap_id] = true;
        st.value[cap_id] = value;
        any = true;
    }
    if (!any) return;

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

/* Publishes the retained state payload for one registry device, qos 0: one
 * field per capability slot the device currently reports (`d->caps[i].valid`
 * -- NOT gated by any plant binding, unlike publish_state() above: spec
 * Sec.6's device state topic is "same idea for devices, bound or not").
 * A device with no valid capability slot yet publishes nothing, same
 * "nothing to say yet" contract as publish_state(). */
static void publish_device_state(const device_entry_t *d)
{
    if (!s_client || !s_mqtt_connected) return;   /* QoS 0: a publish while disconnected is pure waste -- see s_mqtt_connected's comment */

    mqtt_state_t st = { 0 };
    bool any = false;
    for (uint8_t cap_id = 0; cap_id < CAPABILITY_COUNT; cap_id++) {
        if (!d->caps[cap_id].valid) continue;
        st.present[cap_id] = true;
        st.value[cap_id] = capability_decode(cap_id, d->caps[cap_id].raw);
        any = true;
    }
    if (!any) return;

    char idstr[24];
    device_id_format(&d->id, idstr, sizeof(idstr));

    char topic[TOPIC_BUF_SIZE];
    char json[STATE_JSON_BUF_SIZE];
    if (!mqtt_topic_device_state(topic, sizeof(topic), s_hub, idstr)) {
        ESP_LOGW(TAG, "device state topic for %s did not fit", idstr);
        return;
    }
    if (!mqtt_json_state(json, sizeof(json), &st)) {
        ESP_LOGW(TAG, "device state payload for %s did not fit", idstr);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, json, 0, 0, 1);
}

/* True when ANY plant currently binds dev's cap_id -- the dedup test spec
 * Sec.6 (amended) requires before publishing a device-form discovery
 * entity: a capability already exposed through a plant binding must not
 * also get a device-form entity (would surface the same reading twice in
 * HA). Mirrors devices_json.c's add_plant_ids(), one capability at a time
 * instead of "any capability". */
static bool cap_bound_by_plant(const plants_table_t *plants, const device_id_t *dev, uint8_t cap_id)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *p = &plants->p[i];
        if (!p->in_use) continue;
        if (p->cap_bound[cap_id] && device_id_equal(&p->cap_dev[cap_id], dev)) return true;
    }
    return false;
}

/* Publishes device-form HA discovery configs for `d`'s registry slot
 * `dev_slot` (registry_find()'s index, stable for the device's lifetime --
 * see s_device_disc_sent's doc comment), one per capability the device
 * currently reports (`d->caps[i].valid`) that is NOT already covered by a
 * plant binding (cap_bound_by_plant()) -- spec Sec.6's amended dedup rule.
 * Self-healing both ways, so a dropped/missed transition message
 * (mqtt_pub_device_cap_bound(), best-effort like every other cleanup entry
 * point here) is corrected the next time this runs for the device: a
 * capability newly covered by a plant that still has a stale "sent" bit
 * gets its device-form entity cleared here (not just at the
 * DEVICE_CAP_BOUND site); one no longer covered (rebind moved it to a
 * different device, or unbound) but never sent gets published. */
/* QoS 0 throughout (hardware round 4 -- same reasoning as publish_discovery()
 * above, applied here too since this shares the same loop/burst shape: up to
 * CAPABILITY_COUNT publishes per device, across up to REGISTRY_MAX_DEVICES
 * devices in one RESYNC_DISCOVERY sweep). The "covered but stale bit"
 * self-heal cleanup branch is itself just a backstop for
 * cleanup_device_cap_discovery() (still QoS 1, single message, the
 * AUTHORITATIVE cleanup at the actual bind-transition moment) -- if this
 * opportunistic QoS-0 retry is ever lost, the stale bit stays stale and this
 * function simply retries it again next time it runs for the device, so
 * losing it costs nothing. Gated on s_mqtt_connected (re-checked every
 * iteration, same as publish_discovery()) and paced
 * (MQTT_DISCOVERY_PACE_MS) between every publish in the loop, both branches
 * alike. */
static void publish_device_discovery(const device_entry_t *d, int dev_slot, const plants_table_t *plants)
{
    if (!s_client) return;

    char idstr[24];
    device_id_format(&d->id, idstr, sizeof(idstr));

    char name[PLANT_NAME_LEN + 1] = "";
    if (d->id.kind == DEV_KIND_BLE) {
        app_config_get_sensor_name(d->id.addr, name);   /* leaves name[0]=='\0' on failure/unset */
    }

    char topic[TOPIC_BUF_SIZE];
    char json[DISC_JSON_BUF_SIZE];
    for (uint8_t cap_id = 0; cap_id < CAPABILITY_COUNT; cap_id++) {
        if (!s_mqtt_connected) break;   /* disconnected mid-burst: stop, don't waste the rest */
        if (!d->caps[cap_id].valid) continue;

        bool covered = cap_bound_by_plant(plants, &d->id, cap_id);
        if (covered) {
            if (s_device_disc_sent[dev_slot][cap_id]) {
                if (mqtt_topic_device_discovery(topic, sizeof(topic), idstr, cap_id)) {
                    esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 0, 1);
                    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_PACE_MS));
                }
                s_device_disc_sent[dev_slot][cap_id] = false;
            }
            continue;
        }
        if (s_device_disc_sent[dev_slot][cap_id]) continue;   /* already published this session */

        if (!mqtt_topic_device_discovery(topic, sizeof(topic), idstr, cap_id)) {
            ESP_LOGW(TAG, "device discovery topic for %s/cap %u did not fit", idstr, cap_id);
            continue;
        }
        if (!mqtt_json_device_discovery(json, sizeof(json), s_hub, idstr, name, cap_id)) {
            ESP_LOGW(TAG, "device discovery payload for %s/cap %u did not fit", idstr, cap_id);
            continue;
        }
        esp_mqtt_client_publish(s_client, topic, json, 0, 0, 1);
        s_device_disc_sent[dev_slot][cap_id] = true;
        vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_PACE_MS));
    }
}

/* Publishes discovery configs for every plant currently in the plants table
 * (whether or not any capability has reported yet -- discovery just
 * declares the HA entity, and per-plant state publishing already only
 * happens once that plant has actually reported data), then for every
 * registry device's not-yet-plant-covered capabilities (spec Sec.6,
 * amended -- publish_device_discovery()'s dedup) -- marks their bitmap
 * slots sent, so mqtt_pub_task() itself doesn't redundantly resend them
 * the first time each plant/device's next update comes through. Runs on
 * mqtt_pub_task in response to a RESYNC_DISCOVERY message.
 *
 * Takes `reg`/`plants` BY POINTER from the CALLER's own already-live
 * registry_t/plants_table_t (mqtt_pub_task()'s reg_snap/plants_snap)
 * rather than snapshotting its own copy of either: a second ~2KB
 * registry_t + ~2KB plants_table_t pair on top of mqtt_pub_task()'s own
 * already-permanent pair would very nearly repeat the exact stack
 * overflow MQTT_PUB_TASK_STACK's own comment documents (this function
 * used to take its own private plants_table_t snapshot before Task 7's
 * device-discovery amendment added the registry_t need too -- worth
 * folding into the same "reuse the caller's" fix rather than adding a
 * second duplicate snapshot next to the first). */
static void publish_all_discovery(const registry_t *reg, const plants_table_t *plants)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (!plants->p[i].in_use) continue;
        publish_discovery(plants->p[i].id, plants->p[i].name);
        s_discovery_sent[i] = true;
        s_slot_plant_id[i] = plants->p[i].id;
        strncpy(s_slot_name[i], plants->p[i].name, PLANT_NAME_LEN);
        s_slot_name[i][PLANT_NAME_LEN] = '\0';
    }
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        if (!reg->devices[i].in_use) continue;
        publish_device_discovery(&reg->devices[i], i, plants);
    }
}

/* Retained-topic cleanup (spec Sec.6, M2 Task 7): an empty retained payload
 * (MQTT_CLEANUP_PAYLOAD, mqtt_json.h) makes the broker delete the retained
 * message, so a stale entity/value disappears from HA instead of sitting
 * there forever. cleanup_plant() clears every POSSIBLE discovery topic
 * (cap_id 0..CAPABILITY_COUNT-1), not just whatever happened to be bound at
 * delete time: plants_delete() has already dropped the plant's binding
 * table by the time mqtt_pub_plant_deleted() is enqueued (api_v1.c), so
 * there is nothing left to consult for "what was actually published" --
 * clearing every possible topic is cheap (at most 8 retained-delete
 * publishes) and guarantees no orphan survives. Publishing an empty
 * retained payload to a topic that was never actually populated is a
 * harmless no-op on the broker/HA side. */
static void cleanup_plant(uint8_t plant_id)
{
    if (!s_client) return;

    char topic[TOPIC_BUF_SIZE];
    if (mqtt_topic_state(topic, sizeof(topic), s_hub, plant_id)) {
        esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 0, 1);
    }
    for (uint8_t cap_id = 0; cap_id < CAPABILITY_COUNT; cap_id++) {
        if (mqtt_topic_discovery(topic, sizeof(topic), plant_id, cap_id)) {
            esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 1, 1);
        }
    }
}

/* Unbinding one capability only clears ITS discovery entity -- the plant's
 * state topic is untouched (it just stops carrying that field on the next
 * publish; the other bound capabilities' entities are unaffected). */
static void cleanup_cap(uint8_t plant_id, uint8_t cap_id)
{
    if (!s_client) return;

    char topic[TOPIC_BUF_SIZE];
    if (mqtt_topic_discovery(topic, sizeof(topic), plant_id, cap_id)) {
        esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 1, 1);
    }
}

/* Device removal clears only the state topic -- NOT every possible
 * device-form discovery topic the way cleanup_plant() does for a plant:
 * unlike a plant delete (which drops the whole binding table at once, so
 * there is nothing left to consult), a removed device's discovery topics
 * are exactly whatever publish_device_discovery() has actually sent, and
 * that self-heals on its own the next RESYNC_DISCOVERY once the device is
 * gone from the registry (its `in_use` loop just stops visiting it) --
 * clearing them here too would need the caller to still have the device's
 * entry around to know which capabilities it had, defeating the point of
 * "removed". */
static void cleanup_device(const device_id_t *dev)
{
    if (!s_client) return;

    char idstr[24];
    device_id_format(dev, idstr, sizeof(idstr));
    char topic[TOPIC_BUF_SIZE];
    if (mqtt_topic_device_state(topic, sizeof(topic), s_hub, idstr)) {
        esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 0, 1);
    }
}

/* A device capability just became covered by a plant binding (api_v1.c's
 * plants_bind_post(), the bind-with-both-cap-and-device branch) -- its
 * device-form discovery entity, if one was ever sent, is now a duplicate
 * of the plant-form one and must be cleared (spec Sec.6, amended: "a
 * probe's entity migrates from the device form to the plant form when it
 * is bound"). Always publishes the empty retained payload regardless of
 * whether this device actually had one sent (harmless no-op on the
 * broker/HA side if not, same reasoning cleanup_plant() gives for its
 * every-possible-topic sweep) -- simpler and just as cheap as looking up
 * the device's registry slot here just to check s_device_disc_sent first;
 * publish_device_discovery()'s own "covered but bit still set" branch is
 * the belt-and-braces backstop if this message is ever dropped (queue
 * full) or arrives before the device's own registry slot bit gets set. */
static void cleanup_device_cap_discovery(const device_id_t *dev, uint8_t cap_id)
{
    if (!s_client) return;

    char idstr[24];
    device_id_format(dev, idstr, sizeof(idstr));
    char topic[TOPIC_BUF_SIZE];
    if (mqtt_topic_device_discovery(topic, sizeof(topic), idstr, cap_id)) {
        esp_mqtt_client_publish(s_client, topic, MQTT_CLEANUP_PAYLOAD, 0, 1, 1);
    }
}

/* The one task that ever calls esp_mqtt_client_publish() for state/
 * discovery/cleanup traffic. Drains s_queue: a STATE_UPDATE resolves the
 * reporting mac to its plant (plants_resolve_or_create() -- task context,
 * allowed; this is one of the sanctioned lazy-create call sites, since a
 * DATA_EVENT mac is always a real registry mac by construction), throttles
 * to one state publish per plant slot per 30s, and sends discovery first
 * whenever that slot hasn't seen it this MQTT session OR the plant's name
 * has changed since the cached one (a rename must re-publish discovery --
 * see s_slot_name's comment). The SAME throttled pass also publishes the
 * reporting DEVICE's own state (publish_device_state()) and re-evaluates
 * its device-form discovery entities (publish_device_discovery()) --
 * device state/discovery piggyback on the plant-triggering event
 * (CRITICAL fix, Task 7 review: this was the one path that could ever
 * drive planthub/<hub>/device/<id>/state, and it was missing entirely) --
 * see the STATE_UPDATE branch below for why gating both off the same
 * per-plant-slot throttle is sound: every mac that reaches this branch is
 * already a live registry mac (data_core.c only posts DATA_EVENT for one
 * it just wrote), and plants_resolve_or_create() unconditionally
 * auto-creates a plant for it, so 1 mac : 1 plant-slot-throttle-window
 * holds every time this path runs.
 *
 * A RESYNC_DISCOVERY (posted by mqtt_event_handler() right after
 * MQTT_EVENT_CONNECTED, see the file header) walks the whole plants table
 * AND the whole registry, (re)sending discovery for every plant and every
 * not-yet-plant-covered device capability; PLANT_DELETED/CAP_UNBOUND/
 * DEVICE_REMOVED/DEVICE_CAP_BOUND (posted by api_v1.c, M2 Task 7) each
 * publish the matching retained-cleanup payload(s) -- both the per-plant
 * discovery publishes and the full-table resync walk used to happen
 * inline on esp-mqtt's own task; they happen here now instead.
 * registry_t/plants_table_t (~2KB each) live on this task's own stack --
 * fine per the plan, this is the one place in the component that does it
 * (publish_all_discovery() reuses these same two rather than taking its
 * own copies -- see its doc comment). */
static void mqtt_pub_task(void *arg)
{
    (void)arg;
    mqtt_pub_msg_t msg;
    registry_t reg_snap;
    plants_table_t plants_snap;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.type == MQTT_PUB_MSG_RESYNC_DISCOVERY) {
            data_core_snapshot(&reg_snap);
            plants_snapshot(&plants_snap);
            publish_all_discovery(&reg_snap, &plants_snap);
            continue;
        }

        if (msg.type == MQTT_PUB_MSG_EVENT) {
            /* QoS 0 (V1's own convention, unchanged) -- same "pure waste
             * while disconnected" reasoning as the state/discovery paths
             * (s_mqtt_connected's comment); this one is a single message
             * per event, not a loop, but the gate costs nothing to add. */
            if (s_client && s_mqtt_connected) {
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

        if (msg.type == MQTT_PUB_MSG_PLANT_DELETED) {
            cleanup_plant(msg.plant_id);
            continue;
        }

        if (msg.type == MQTT_PUB_MSG_CAP_UNBOUND) {
            cleanup_cap(msg.plant_id, msg.cap_id);
            continue;
        }

        if (msg.type == MQTT_PUB_MSG_DEVICE_REMOVED) {
            cleanup_device(&msg.dev);
            continue;
        }

        if (msg.type == MQTT_PUB_MSG_DEVICE_CAP_BOUND) {
            cleanup_device_cap_discovery(&msg.dev, msg.cap_id);
            /* H2 fixwave (spec Sec.6: "a probe's entity migrates from the
             * device form to the plant form when it is bound"): the cleanup
             * above only removes the stale device-form entity. Without this,
             * the plant-form entity for msg.cap_id never gets (re)published
             * here -- publish_discovery()'s only other call site is gated on
             * `!s_discovery_sent[slot] || name_changed`, which is already
             * false for a plant that published discovery earlier this
             * session, so the newly-bound capability would have no entity in
             * EITHER form until a reconnect/reboot/rename. Republish this
             * plant's whole discovery set immediately (still solely from
             * this task -- single-publisher discipline preserved) and
             * refresh the same bookkeeping publish_all_discovery() would,
             * so the periodic/state-triggered paths don't redundantly resend
             * it. plants_snap is this task's own permanent stack copy (see
             * mqtt_pub_task()'s doc comment); re-snapshotting it here is
             * just a fresh read, not an extra allocation. */
            plants_snapshot(&plants_snap);
            int bound_slot = plants_table_find_id(&plants_snap, msg.plant_id);
            if (bound_slot >= 0) {
                const char *bound_name = plants_snap.p[bound_slot].name;
                publish_discovery(msg.plant_id, bound_name);
                s_discovery_sent[bound_slot] = true;
                s_slot_plant_id[bound_slot] = msg.plant_id;
                strncpy(s_slot_name[bound_slot], bound_name, PLANT_NAME_LEN);
                s_slot_name[bound_slot][PLANT_NAME_LEN] = '\0';
            }
            continue;
        }

        uint8_t plant_id = plants_resolve_or_create(msg.mac);
        if (plant_id == 0) continue;   /* plants table full; already logged once/mac/boot */

        data_core_snapshot(&reg_snap);
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
        publish_state(plant_id, &reg_snap);

        /* Device state/discovery for the SAME device that just triggered
         * this whole branch (msg.mac -- BLE-only today, same assumption
         * data_core.c's DATA_EVENT producer and sse.c's on_sensor_update()
         * both already make: "every current producer is BLE-kind"). Reuses
         * reg_snap/plants_snap already taken above -- no extra registry
         * walk or snapshot. registry_find() (not plants_resolve_or_create,
         * which is mac->PLANT) gives this device's own stable registry
         * slot, required by publish_device_discovery()'s s_device_disc_sent
         * indexing. */
        device_id_t dev_id = device_id_from_mac(DEV_KIND_BLE, msg.mac);
        int dev_slot = registry_find(&reg_snap, &dev_id);
        if (dev_slot >= 0) {
            publish_device_state(&reg_snap.devices[dev_slot]);
            publish_device_discovery(&reg_snap.devices[dev_slot], dev_slot, &plants_snap);
        }
    }
}

/* PLANTHUB_DATA_EVENT/DATA_EVENT_SENSOR_UPDATE handler -- default event
 * loop task. Callback discipline: queue-and-return only, see the file
 * header. Drop-HEAD on a full queue (per the plan; M4 fixwave correction --
 * this used to be described as "the newest update is kept rather than the
 * stalest one", which is only true when every item happens to be a
 * STATE_UPDATE. xQueueReceive() always takes the queue's FRONT item, which
 * on a busy queue can just as easily be a pending RESYNC_DISCOVERY/
 * PLANT_DELETED/CAP_UNBOUND/DEVICE_REMOVED/DEVICE_CAP_BOUND control message
 * as a stale sensor reading. Left as a plain FIFO head-drop rather than
 * reworked to skip control messages: every other producer into this same
 * queue (mqtt_pub_plant_deleted()/mqtt_pub_cap_unbound()/
 * mqtt_pub_device_removed()/mqtt_pub_device_cap_bound(), all above) already
 * documents its own send as best-effort/self-healing on a full queue --
 * RESYNC_DISCOVERY re-derives the whole discovery set from scratch next
 * reconnect, and the retained-cleanup messages' own comments already accept
 * "a missed cleanup leaves a stale HA entity a little longer" -- so an
 * occasional head-drop landing on one of them is within the tradeoff this
 * file already makes elsewhere, not a new risk. If the dropped head is a
 * MQTT_PUB_MSG_EVENT, its heap-allocated event_json (mqtt_pub_event()'s
 * strdup) must still be freed here -- the two other paths that ever take an
 * EVENT message off this queue (the normal publish/free after
 * MQTT_PUB_MSG_EVENT above, and mqtt_pub_event()'s own xQueueSend-failed
 * path) both already free it; this is the one spot that used to leak it. */
static void data_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_id;
    if (!s_queue || !event_data) return;

    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_STATE_UPDATE };
    memcpy(msg.mac, event_data, sizeof(msg.mac));

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        mqtt_pub_msg_t discard;
        if (xQueueReceive(s_queue, &discard, 0) == pdTRUE && discard.type == MQTT_PUB_MSG_EVENT) {
            free(discard.event_json);
        }
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
        s_mqtt_connected = true;   /* set before RESYNC_DISCOVERY is enqueued below, so mqtt_pub_task sees it true from the start of that burst */
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
        /* s_device_disc_sent is heap now, not a static array -- sizeof() on
         * the pointer itself would be wrong (pointer width, not the
         * REGISTRY_MAX_DEVICES*CAPABILITY_COUNT bools behind it). NULL-safe:
         * this handler can only run after start_mqtt() succeeded, which is
         * the only place s_device_disc_sent is ever assigned (never freed). */
        memset(s_device_disc_sent, 0, REGISTRY_MAX_DEVICES * sizeof(*s_device_disc_sent));
        if (s_queue) {
            mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_RESYNC_DISCOVERY };
            if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "mqtt_pub queue full; skipping this reconnect's discovery resync");
            }
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
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

    /* Heap, not static -- see s_device_disc_sent's own declaration comment.
     * calloc() zero-initialises, matching a static array's implicit
     * zero-init (every capability starts "not yet sent", correctly). */
    s_device_disc_sent = calloc(REGISTRY_MAX_DEVICES, sizeof(*s_device_disc_sent));
    if (!s_device_disc_sent) {
        ESP_LOGE(TAG, "device-discovery bitmap allocation failed");
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(MQTT_PUB_QUEUE_DEPTH, sizeof(mqtt_pub_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        free(s_device_disc_sent);
        s_device_disc_sent = NULL;
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
    /* Explicit here, not left to CONFIG_MQTT_USE_CUSTOM_CONFIG's Kconfig
     * defaults (also set, sdkconfig.defaults -- see that file's comment):
     * esp-mqtt's esp_mqtt_set_config() always prefers a non-zero struct
     * field over the compiled-in Kconfig default regardless of
     * MQTT_USE_CUSTOM_CONFIG (see mqtt_client.c), so setting it here is
     * both the authoritative value AND immune to the generated-sdkconfig-
     * doesn't-pick-up-defaults-file gotcha that bit the buffer/stack
     * settings the first time. See MQTT_CLIENT_TASK_STACK/_IN_BUF_SIZE/
     * _OUT_BUF_SIZE's own comment for the sizing justification. */
    mcfg.task.stack_size   = MQTT_CLIENT_TASK_STACK;
    mcfg.buffer.size       = MQTT_CLIENT_IN_BUF_SIZE;
    mcfg.buffer.out_size   = MQTT_CLIENT_OUT_BUF_SIZE;

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

/* Retained-topic cleanup entry points (spec Sec.6, M2 Task 7) -- api_v1.c
 * calls these right after plants_delete()/plants_bind_cap() succeeds; see
 * cleanup_plant()/cleanup_cap()/cleanup_device()/
 * cleanup_device_cap_discovery() above for what each actually publishes.
 * Same "no header of its own", "queue and return, no-op when MQTT isn't
 * running" conventions as mqtt_pub_event() above. Best-effort on a full
 * queue (logged, dropped) -- same rationale as mqtt_pub_event(): a missed
 * cleanup leaves a stale HA entity a little longer, never a wrong/
 * duplicated one -- and for mqtt_pub_device_cap_bound() specifically,
 * publish_device_discovery()'s own "covered but bit still set" branch is
 * a second, self-healing chance to catch what this one dropped. */
void mqtt_pub_plant_deleted(uint8_t plant_id)
{
    if (!s_client || !s_queue) return;
    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_PLANT_DELETED, .plant_id = plant_id };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_pub queue full; dropping plant %u delete cleanup", plant_id);
    }
}

void mqtt_pub_cap_unbound(uint8_t plant_id, uint8_t cap_id)
{
    if (!s_client || !s_queue) return;
    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_CAP_UNBOUND, .plant_id = plant_id, .cap_id = cap_id };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_pub queue full; dropping plant %u cap %u unbind cleanup", plant_id, cap_id);
    }
}

/* No current caller: the M2 registry (registry.h) never evicts a device
 * entry once created ("registry.c never evicts an entry" -- see
 * plants.h's plants_adopt_from_registry() doc comment), so there is no
 * "device removed" event anywhere yet in this milestone. Implemented now
 * (spec Sec.6 lists it alongside plant-delete/cap-unbind as a retained-
 * cleanup trigger) so a future milestone that adds device eviction only
 * needs to call this, not design the cleanup itself. */
void mqtt_pub_device_removed(const device_id_t *dev)
{
    if (!s_client || !s_queue || !dev) return;
    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_DEVICE_REMOVED, .dev = *dev };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_pub queue full; dropping device removal cleanup");
    }
}

/* dev's cap_id just became bound to plant_id (api_v1.c's plants_bind_post(),
 * the bind branch -- cap AND device both given) -- see
 * cleanup_device_cap_discovery()'s doc comment for what this clears and
 * why, and mqtt_pub_task()'s MQTT_PUB_MSG_DEVICE_CAP_BOUND branch (H2
 * fixwave) for the plant-form republish plant_id drives so the entity
 * actually migrates rather than just disappearing from the device form.
 * NOT called on a plain rebind of an already-covered capability (still
 * covered, same cleanup would be a harmless no-op, but there is nothing new
 * to clear); api_v1.c calls this on every successful bind regardless, since
 * "was this capability already covered by some OTHER plant before this
 * call" isn't something plants_bind_cap()'s return value tells the caller,
 * and re-clearing an already-cleared/never-sent topic is free. */
void mqtt_pub_device_cap_bound(const device_id_t *dev, uint8_t cap_id, uint8_t plant_id)
{
    if (!s_client || !s_queue || !dev) return;
    mqtt_pub_msg_t msg = { .type = MQTT_PUB_MSG_DEVICE_CAP_BOUND, .dev = *dev, .cap_id = cap_id, .plant_id = plant_id };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_pub queue full; dropping device cap %u bound cleanup", cap_id);
    }
}
