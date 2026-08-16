/* InfluxDB v2 line-protocol batch push -- Task 5 of the M6 integrations
 * plan, generalised onto the capability table by M2 Task 7 (spec Sec.6):
 * one `plant` line per plant with any live bound-capability value, one
 * `device` line per registry device with any live capability value --
 * field names from capability.h's influx_field (V1 spellings kept:
 * moisture, temp, lux, conductivity, battery; humidity/pressure/rssi are
 * new). influx_start() is called unconditionally from integrations_start()
 * (mqtt_pub.c), the same way it calls start_mqtt(): this file applies its
 * own influx.enabled check, exactly like start_mqtt() handles mqtt.enabled.
 *
 * Design: one task, one line-protocol batch buffer (heap-allocated only
 * when enabled -- see below), one esp_http_client POST every 300s. No
 * retry queue on a failed POST -- the
 * next cycle re-reports whatever is still current in the registry, which
 * is simpler and correct enough (YAGNI, per the plan) since an Influx
 * point is last-value-wins per (measurement, tag set, timestamp) anyway.
 * Both plant and device lines share ONE timestamp per cycle (epoch_now,
 * this cycle's wall-clock read) rather than V1's per-point "back-date by
 * this device's staleness" -- V1 had exactly one device behind each point,
 * so its own age was an unambiguous timestamp; a plant can now draw
 * capabilities from several devices with different ages, so there is no
 * single honest back-dated timestamp for the point as a whole. Individual
 * capabilities that have gone stale (age_s > DATA_CORE_MAX_AGE_S) are still
 * excluded, same intent as V1's staleness skip, just per-capability instead
 * of per-device.
 *
 * cfg is received by pointer at influx_start() time and the caller's
 * struct is stack-local (mqtt_pub.c's integrations_start()), so the parts
 * this file needs are copied into a HEAP-allocated block before the task
 * is spawned -- see s_cfg/s_batch/s_url/s_auth_header's own comment
 * (M2 Task 7 hardware round: these used to be plain statics, paid by every
 * hub's .bss whether or not Influx was ever turned on; on a C3 juggling
 * WiFi+BLE+MQTT in ~300 KB of DRAM, an unconditional 8.6 KB was enough on
 * its own to starve esp-mqtt of a usable free block -- see influx_start()).
 */
#include "integr_private.h"
#include "lineproto.h"
#include "data_core.h"
#include "registry.h"
#include "capability.h"
#include "plants.h"
#include "plants_table.h"
#include "timekeeper.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "influx";

#define INFLUX_TASK_STACK 6144   /* TLS-ready headroom, see the plan */
#define INFLUX_TASK_PRIO   2
#define INFLUX_INTERVAL_S  300
/* 8192, not V1's 4096 (M2 Task 7): this file now emits up to
 * REGISTRY_MAX_DEVICES (16) `device` lines in addition to up to PLANTS_MAX
 * (16) `plant` lines -- an entirely new class of lines V1 never had --
 * where before it emitted only up to 16 assigned-plant lines. Worst case
 * (every line carrying all CAPABILITY_COUNT=8 fields) no longer fits 4096;
 * 8192 covers 32 worst-case lines (~215 B each) with headroom. This used to
 * cost every hub +4096 B of static .bss over V1 unconditionally; as of the
 * hardware round below it's a heap allocation instead, made only when
 * Influx is actually enabled. */
#define INFLUX_BATCH_CAP   8192
#define INFLUX_URL_CAP     256   /* url[97] + "/api/v2/write?org=" + org[33] + "&bucket=" + bucket[33] + "&precision=s" */
#define INFLUX_AUTH_CAP    160   /* "Token " + token[129] */

/* Config copy + working buffers: everything the task needs lives here, none
 * of it on the task's own stack and none of it allocated per-cycle.
 *
 * HEAP, not static (M2 Task 7 hardware round -- see task-7-report.md's
 * heap-reclaim section): a static array here costs every hub's .bss the
 * same whether or not integr_config_t.influx.enabled is ever true, and on
 * the C3 that was the single biggest contributor to a heap regression that
 * broke MQTT (esp-mqtt couldn't get a usable free block, "delayed connect
 * error" reconnect-storming the WiFi link) -- on a board with Influx
 * disabled (most hubs, including the one that reproduced this), that
 * 8.6 KB combined was pure waste. malloc'd once, here, only when
 * cfg->influx.enabled (never freed -- there is no runtime "turn Influx
 * off" path; a config change reboots the hub, see integr_config_set()'s
 * callers, so this is a one-shot cost for the process lifetime, exactly
 * like the static it replaces, just conditional instead of unconditional).
 * NULL on a disabled board for the rest of the process's life; every
 * reader of these four pointers (build_batch()/post_batch()/influx_task())
 * only ever runs on influx_task, which itself is never created unless
 * they were all successfully allocated first -- see influx_start(). */
static integr_config_t *s_cfg;
static char *s_batch;
static char *s_url;
static char *s_auth_header;

/* Appends one plant's line, if it has at least one live bound-capability
 * value: walks plant_id's current bindings (plants_bindings()) and reads
 * each through plants_cap_value() -- which already does the
 * binding->device->cap_slot_t resolution against `reg` -- skipping a
 * capability that's unbound-in-practice (bound device not in this
 * snapshot, or no value yet) or has aged out. */
static void append_plant(uint8_t plant_id, const registry_t *reg, int64_t epoch_now, size_t *off)
{
    plant_binding_t bindings[CAPABILITY_COUNT];
    size_t n = plants_bindings(plant_id, bindings, CAPABILITY_COUNT);
    if (n == 0) return;

    lp_plant_point_t p = { .plant_id = plant_id, .epoch_s = epoch_now };
    for (size_t i = 0; i < n; i++) {
        uint8_t cap_id = bindings[i].cap_id;
        float value;
        uint32_t age_s;
        if (!plants_cap_value(plant_id, cap_id, reg, &value, &age_s)) continue;
        if (age_s > DATA_CORE_MAX_AGE_S) continue;
        p.fields.present[cap_id] = true;
        p.fields.value[cap_id] = value;
    }

    if (!lineproto_append_plant(s_batch, INFLUX_BATCH_CAP, off, &p)) {
        ESP_LOGD(TAG, "line for plant %u did not fit or had no fields; skipped", plant_id);
    }
}

/* Appends one device's line, if it has at least one live capability value
 * -- every registry device, bound to a plant or not (spec Sec.6: "measurement
 * device ... field per capability", the plants-primary/devices-secondary
 * decision means devices get reported independently of plant bindings). */
static void append_device(const device_entry_t *d, uint32_t now_uptime_s, int64_t epoch_now, size_t *off)
{
    lp_device_point_t p = { .epoch_s = epoch_now };
    device_id_format(&d->id, p.dev_id_str, sizeof(p.dev_id_str));

    for (uint8_t cap_id = 0; cap_id < CAPABILITY_COUNT; cap_id++) {
        const cap_slot_t *slot = &d->caps[cap_id];
        if (!slot->valid) continue;
        uint32_t age_s = (now_uptime_s >= slot->updated_s) ? now_uptime_s - slot->updated_s : 0;
        if (age_s > DATA_CORE_MAX_AGE_S) continue;
        p.fields.present[cap_id] = true;
        p.fields.value[cap_id] = capability_decode(cap_id, slot->raw);
    }

    if (!lineproto_append_device(s_batch, INFLUX_BATCH_CAP, off, &p)) {
        ESP_LOGD(TAG, "line for device %s did not fit or had no fields; skipped", p.dev_id_str);
    }
}

/* Snapshots the plants table and the registry once, then appends every
 * plant's and every device's line (see append_plant()/append_device()
 * above). Returns the number of bytes written to s_batch (0 == nothing to
 * send). */
static size_t build_batch(void)
{
    plants_table_t plants_snap;
    plants_snapshot(&plants_snap);

    registry_t reg_snap;
    data_core_snapshot(&reg_snap);

    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    int64_t epoch_now = (int64_t)timekeeper_now();
    size_t off = 0;

    for (int i = 0; i < PLANTS_MAX; i++) {
        if (!plants_snap.p[i].in_use) continue;
        append_plant(plants_snap.p[i].id, &reg_snap, epoch_now, &off);
    }

    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        if (!reg_snap.devices[i].in_use) continue;
        append_device(&reg_snap.devices[i], now_uptime_s, epoch_now, &off);
    }

    return off;
}

/* POSTs s_batch (len bytes) to <url>/api/v2/write?org=<org>&bucket=<bucket>&precision=s.
 * 204 is the only success status; anything else (including a transport
 * error) is logged at WARN and the batch is dropped -- no retry queue. */
static void post_batch(size_t len)
{
    snprintf(s_url, INFLUX_URL_CAP, "%s/api/v2/write?org=%s&bucket=%s&precision=s",
             s_cfg->influx.url, s_cfg->influx.org, s_cfg->influx.bucket);
    snprintf(s_auth_header, INFLUX_AUTH_CAP, "Token %s", s_cfg->influx.token);

    esp_http_client_config_t http_cfg = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGW(TAG, "esp_http_client_init failed; dropping batch");
        return;
    }

    esp_http_client_set_header(client, "Authorization", s_auth_header);
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_post_field(client, s_batch, (int)len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "influx POST transport error: %s; dropping batch", esp_err_to_name(err));
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status == 204) {
            ESP_LOGD(TAG, "influx POST ok (%d bytes)", (int)len);
        } else {
            ESP_LOGW(TAG, "influx POST failed: status %d; dropping batch", status);
        }
    }

    esp_http_client_cleanup(client);
}

static void influx_task(void *arg)
{
    (void)arg;
    bool warned_unsynced = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INFLUX_INTERVAL_S * 1000));

        if (!timekeeper_synced()) {
            if (!warned_unsynced) {
                ESP_LOGW(TAG, "clock not synced yet; skipping cycle");
                warned_unsynced = true;
            } else {
                ESP_LOGD(TAG, "clock not synced yet; skipping cycle");
            }
            continue;
        }

        size_t len = build_batch();
        if (len == 0) {
            ESP_LOGD(TAG, "empty batch; skipping POST");
            continue;
        }

        post_batch(len);
    }
}

esp_err_t influx_start(const integr_config_t *cfg)
{
    if (!cfg->influx.enabled) return ESP_OK;

#ifndef CONFIG_PLANTHUB_INFLUX_TLS
    /* integr_config_set() already rejects https:// URLs when TLS support
     * isn't compiled in, but defend against a config blob written by a
     * TLS-enabled build (or a future edit to that validation) reaching a
     * TLS-disabled binary: never let esp_http_client attempt a scheme this
     * build cannot actually speak. */
    if (strncmp(cfg->influx.url, "https://", 8) == 0) {
        ESP_LOGE(TAG, "influx url is https:// but PLANTHUB_INFLUX_TLS is off; refusing to start");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    /* Heap, not static -- see s_cfg's own declaration comment. Allocated
     * only on this (the enabled) path; a disabled board never calls
     * malloc() here at all, and pays exactly 0 bytes for any of the four. */
    s_cfg = malloc(sizeof(*s_cfg));
    s_batch = malloc(INFLUX_BATCH_CAP);
    s_url = malloc(INFLUX_URL_CAP);
    s_auth_header = malloc(INFLUX_AUTH_CAP);
    if (!s_cfg || !s_batch || !s_url || !s_auth_header) {
        ESP_LOGE(TAG, "influx buffer allocation failed (%u B requested); influx disabled this boot",
                 (unsigned)(sizeof(*s_cfg) + INFLUX_BATCH_CAP + INFLUX_URL_CAP + INFLUX_AUTH_CAP));
        free(s_cfg); free(s_batch); free(s_url); free(s_auth_header);
        s_cfg = NULL; s_batch = NULL; s_url = NULL; s_auth_header = NULL;
        return ESP_ERR_NO_MEM;
    }
    *s_cfg = *cfg; /* caller's struct is stack-local; copy before the task outlives it */

    BaseType_t ok = xTaskCreate(influx_task, "influx", INFLUX_TASK_STACK, NULL, INFLUX_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(influx) failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "influx starting: url=%s org=%s bucket=%s", s_cfg->influx.url, s_cfg->influx.org, s_cfg->influx.bucket);
    return ESP_OK;
}
