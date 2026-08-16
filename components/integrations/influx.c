/* InfluxDB v2 line-protocol batch push -- Task 5 of the M6 integrations
 * plan. influx_start() is called unconditionally from integrations_start()
 * (mqtt_pub.c), the same way it calls start_mqtt(): this file applies its
 * own influx.enabled check, exactly like start_mqtt() handles mqtt.enabled.
 *
 * Design: one task, one static 4KB line-protocol batch buffer, one
 * esp_http_client POST every 300s. No retry queue on a failed POST -- the
 * next cycle re-reports whatever is still current in the registry, which
 * is simpler and correct enough (YAGNI, per the plan) since an Influx
 * point is last-value-wins per (measurement, tag set, timestamp) anyway.
 *
 * cfg is received by pointer at influx_start() time and the caller's
 * struct is stack-local (mqtt_pub.c's integrations_start()), so the parts
 * this file needs are copied into statics before the task is spawned.
 */
#include "integr_private.h"
#include "lineproto.h"
#include "data_core.h"
#include "registry.h"
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

static const char *TAG = "influx";

#define INFLUX_TASK_STACK 6144   /* TLS-ready headroom, see the plan */
#define INFLUX_TASK_PRIO   2
#define INFLUX_INTERVAL_S  300
#define INFLUX_BATCH_CAP   4096  /* 16 sensors x ~200B/line fits comfortably */
#define INFLUX_URL_CAP     256   /* url[97] + "/api/v2/write?org=" + org[33] + "&bucket=" + bucket[33] + "&precision=s" */
#define INFLUX_AUTH_CAP    160   /* "Token " + token[129] */

/* Config copy + static buffers: everything the task needs lives here, none
 * of it on the task's own stack and none of it allocated per-cycle. */
static integr_config_t s_cfg;
static char s_batch[INFLUX_BATCH_CAP];
static char s_url[INFLUX_URL_CAP];
static char s_auth_header[INFLUX_AUTH_CAP];

/* Snapshots the plants table and the registry, and appends every assigned
 * plant's live (non-stale) reading as a line-protocol line to s_batch --
 * unassigned plants (no probe) are skipped, as are assigned plants whose
 * probe mac has no registry entry yet (never reported to the hub) or whose
 * last report has aged out. now_uptime_s is esp_timer-based uptime seconds,
 * the same clock last_seen_s is stamped in (see data_core.c); epoch_s per
 * point is computed by walking that age back off the real wall-clock time.
 * Returns the number of bytes written to s_batch (0 == nothing to send). */
static size_t build_batch(void)
{
    plants_table_t plants_snap;
    plants_snapshot(&plants_snap);

    legacy_registry_t reg_snap;   /* M2-SHIM */
    data_core_snapshot_legacy(&reg_snap);   /* M2-SHIM */

    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t epoch_now = timekeeper_now();
    size_t off = 0;

    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *pe = &plants_snap.p[i];
        if (!pe->in_use || !pe->mac_valid) continue;   /* assigned only */

        int ridx = legacy_registry_find(&reg_snap, pe->mac);   /* M2-SHIM */
        if (ridx < 0) continue;   /* probe never reported to this hub */

        const sensor_entry_t *e = &reg_snap.sensors[ridx];
        uint32_t age_s = now_uptime_s - e->last_seen_s;
        if (age_s > DATA_CORE_MAX_AGE_S) continue;

        lp_point_t p = {0};
        p.plant_id = pe->id;
        strncpy(p.name, pe->name, sizeof(p.name) - 1);
        snprintf(p.sensor_mac12, sizeof(p.sensor_mac12), "%02X%02X%02X%02X%02X%02X",
                 pe->mac[0], pe->mac[1], pe->mac[2], pe->mac[3], pe->mac[4], pe->mac[5]);

        p.has_temp = e->latest.has_temp;
        p.temp_c = e->latest.temp_dc / 10.0f;
        p.has_moisture = e->latest.has_moisture;
        p.moisture_pct = e->latest.moisture_pct;
        p.has_lux = e->latest.has_lux;
        p.lux = e->latest.lux;
        p.has_conductivity = e->latest.has_conductivity;
        p.conductivity = e->latest.conductivity_us;
        p.has_battery = e->latest.has_battery;
        p.battery_pct = e->latest.battery_pct;
        p.epoch_s = (int64_t)epoch_now - (int64_t)age_s;

        if (!lineproto_append(s_batch, sizeof(s_batch), &off, &p)) {
            ESP_LOGD(TAG, "line for plant %u did not fit or had no fields; skipped", pe->id);
        }
    }

    return off;
}

/* POSTs s_batch (len bytes) to <url>/api/v2/write?org=<org>&bucket=<bucket>&precision=s.
 * 204 is the only success status; anything else (including a transport
 * error) is logged at WARN and the batch is dropped -- no retry queue. */
static void post_batch(size_t len)
{
    snprintf(s_url, sizeof(s_url), "%s/api/v2/write?org=%s&bucket=%s&precision=s",
             s_cfg.influx.url, s_cfg.influx.org, s_cfg.influx.bucket);
    snprintf(s_auth_header, sizeof(s_auth_header), "Token %s", s_cfg.influx.token);

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

    s_cfg = *cfg; /* caller's struct is stack-local; copy before the task outlives it */

    BaseType_t ok = xTaskCreate(influx_task, "influx", INFLUX_TASK_STACK, NULL, INFLUX_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(influx) failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "influx starting: url=%s org=%s bucket=%s", s_cfg.influx.url, s_cfg.influx.org, s_cfg.influx.bucket);
    return ESP_OK;
}
