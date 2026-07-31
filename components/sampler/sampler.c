#include "sampler.h"
#include "data_core.h"
#include "storage.h"
#include "hourly_agg.h"
#include "timekeeper.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

/* 32 = CACHE_SLOTS in storage.c; this ties the constants until they share a
 * header. If REGISTRY_MAX_SENSORS ever grows, storage's per-tier cache must
 * grow with it or eviction thrash will silently degrade the ring files. */
_Static_assert(REGISTRY_MAX_SENSORS * 2 <= 32, "storage cache slots must cover all sensors x tiers");

static const char *TAG = "sampler";

static const char *s_base;
static SemaphoreHandle_t s_wake;
static hourly_agg_t s_agg[REGISTRY_MAX_SENSORS];
static uint8_t s_agg_mac[REGISTRY_MAX_SENSORS][6];
static bool s_agg_used[REGISTRY_MAX_SENSORS];

static hourly_agg_t *agg_for(const uint8_t mac[6])
{
    int free_i = -1;
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        if (s_agg_used[i] && memcmp(s_agg_mac[i], mac, 6) == 0) return &s_agg[i];
        if (!s_agg_used[i] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    s_agg_used[free_i] = true;
    memcpy(s_agg_mac[free_i], mac, 6);
    hourly_agg_init(&s_agg[free_i]);
    return &s_agg[free_i];
}

static storage_rec_t rec_from_entry(const sensor_entry_t *e, uint32_t rel_s)
{
    storage_rec_t r;
    memset(&r, 0xFF, sizeof(r));
    r.temp_dc = STORAGE_TEMP_NONE;
    r.boot_id = timekeeper_boot_id();
    r.rel_s = rel_s;
    if (e->latest.has_temp)         r.temp_dc = e->latest.temp_dc;
    if (e->latest.has_moisture)     r.moisture_pct = e->latest.moisture_pct;
    if (e->latest.has_battery)      r.battery_pct = e->latest.battery_pct;
    if (e->latest.has_lux)          r.lux = e->latest.lux;
    if (e->latest.has_conductivity) r.conductivity_us = e->latest.conductivity_us;
    return r;
}

static void sample_once(void)
{
    static registry_t snap;   /* only touched on the sampler task */
    data_core_snapshot(&snap);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t interval_s = CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60;

    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        const sensor_entry_t *e = &snap.sensors[i];
        if (!e->in_use) continue;
        /* skip sensors that produced nothing since the last sample (dead battery / gone) */
        if (now - e->last_seen_s > interval_s) continue;

        storage_rec_t rec = rec_from_entry(e, now);
        if (storage_append(s_base, e->mac, STORAGE_TIER_RAW, &rec) != 0) {
            ESP_LOGW(TAG, "raw append failed for sensor %d", i);
            continue;
        }
        hourly_agg_t *agg = agg_for(e->mac);
        storage_rec_t hr;
        if (agg && hourly_agg_add(agg, &rec, &hr)) {
            if (storage_append(s_base, e->mac, STORAGE_TIER_HOURLY, &hr) != 0)
                ESP_LOGW(TAG, "hourly append failed for sensor %d", i);
        }
    }
}

static void sampler_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        sample_once();
    }
}

static void timer_cb(void *arg)
{
    xSemaphoreGive(s_wake);   /* wake the worker; never do file I/O here */
}

esp_err_t sampler_start(const char *base_path)
{
    s_base = base_path;
    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sampler_task, "sampler", 4096, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t t = { .callback = timer_cb, .name = "sampler" };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&t, &timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(timer, (uint64_t)CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60 * 1000000ULL);
}
