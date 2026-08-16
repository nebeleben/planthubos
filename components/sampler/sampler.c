#include "sampler.h"
#include "data_core.h"
#include "storage.h"
#include "storage_compat.h"   /* M2-SHIM */
#include "hourly_agg.h"
#include "timekeeper.h"
#include "plants.h"
#include "plants_table.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

/* 32 = CACHE_SLOTS in storage.c; this ties the constants until they share a
 * header. storage.c's rings (and this file's aggregation state) are keyed
 * by plant id since M8 Task 3/5, so it's PLANTS_MAX that must stay covered
 * -- if PLANTS_MAX ever grows, storage's per-tier cache must grow with it
 * or eviction thrash will silently degrade the ring files. */
_Static_assert(PLANTS_MAX * 2 <= 32, "storage cache slots must cover all plants x tiers");

static const char *TAG = "sampler";

static const char *s_base;
static SemaphoreHandle_t s_wake;
static hourly_agg_t s_agg[PLANTS_MAX];
static uint8_t s_agg_plant_id[PLANTS_MAX];
static bool s_agg_used[PLANTS_MAX];

static hourly_agg_t *agg_for(uint8_t plant_id)
{
    int free_i = -1;
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (s_agg_used[i] && s_agg_plant_id[i] == plant_id) return &s_agg[i];
        if (!s_agg_used[i] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    s_agg_used[free_i] = true;
    s_agg_plant_id[free_i] = plant_id;
    hourly_agg_init(&s_agg[free_i]);
    return &s_agg[free_i];
}

/* M2-SHIM: builds the V1-shaped record; storage_encode_v1() (storage_compat.h)
 * maps it onto the real V2 storage_rec_t below. */
static storage_rec_v1_t rec_from_entry(const sensor_entry_t *e, uint32_t rel_s)
{
    storage_rec_v1_t r;
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
    /* only touched on the sampler task */
    static legacy_registry_t reg_snap;   /* M2-SHIM */
    static plants_table_t plant_snap;
    data_core_snapshot_legacy(&reg_snap);   /* M2-SHIM */
    plants_snapshot(&plant_snap);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t interval_s = CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60;

    /* Self-healing sweep (M8 Task 6 review fix -- MEDIUM 3b, also closes
     * Task 5's deferred "16-slot cache never frees a claimed slot" note):
     * plant ids are never reused (plants_table.h), so a deleted plant's
     * s_agg slot would otherwise sit claimed forever -- past PLANTS_MAX
     * (16) lifetime create/delete cycles, every slot would be permanently
     * pinned to a dead id and hourly aggregation would silently stop for
     * any newer plant. Free any slot whose plant no longer exists in THIS
     * tick's snapshot before sampling; bounded PLANTS_MAX iterations,
     * allocation-free, same cost shape as the rest of this per-tick work.
     * Deliberately no plants -> sampler dependency beyond what already
     * exists here: plants_table_find_id() is a pure function over the
     * snapshot already taken above. */
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (s_agg_used[i] && plants_table_find_id(&plant_snap, s_agg_plant_id[i]) < 0) {
            s_agg_used[i] = false;
        }
    }

    /* Plants are the unit of sampling (M8 Task 5): walk the plant table, not
     * the sensor registry. A plant with no assigned probe (mac_valid ==
     * false) is skipped outright -- it has nothing to sample, per spec. */
    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *p = &plant_snap.p[i];
        if (!p->in_use || !p->mac_valid) continue;

        int ri = legacy_registry_find(&reg_snap, p->mac);   /* M2-SHIM */
        if (ri < 0) {
            /* Assigned to a probe this boot hasn't heard from yet -- there's
             * no registry entry to sample. Not a warning: this is routine
             * right after a reboot or a fresh assignment. */
            ESP_LOGD(TAG, "plant %u: probe not heard yet this boot, skipping", p->id);
            continue;
        }
        const sensor_entry_t *e = &reg_snap.sensors[ri];
        /* skip sensors that produced nothing since the last sample (dead battery / gone) */
        if (now - e->last_seen_s > interval_s) continue;

        storage_rec_v1_t v1 = rec_from_entry(e, now);
        storage_rec_t rec;
        storage_encode_v1(s_base, p->id, &v1, &rec);   /* M2-SHIM */
        if (storage_append(s_base, p->id, STORAGE_TIER_RAW, &rec) != 0) {
            ESP_LOGW(TAG, "raw append failed for plant %u", p->id);
            continue;
        }
        hourly_agg_t *agg = agg_for(p->id);
        storage_rec_t hr;
        if (agg && hourly_agg_add(agg, &rec, &hr)) {
            if (storage_append(s_base, p->id, STORAGE_TIER_HOURLY, &hr) != 0)
                ESP_LOGW(TAG, "hourly append failed for plant %u", p->id);
        }
    }

    /* Auto-create sweep: a mac the registry already knows but no plant has
     * claimed (a new sensor that appeared mid-uptime) gets a plant now, off
     * the same registry snapshot taken above. It is sampled starting the
     * NEXT tick, not this one -- the plant loop above already ran -- which
     * is fine: the alternative (special-casing a just-created plant into
     * this tick) buys one sample interval of latency at the cost of real
     * complexity. Extracted to plants_adopt_from_registry() (final M8
     * review, H1+M3) so api_v1.c's plants_get can drive the identical,
     * liveness-gated sweep too instead of a fresh hub waiting up to
     * interval_s for its first plant to appear -- see that function's doc
     * comment in plants.h for the liveness gate and the DoS-rule boundary
     * (registry-snapshot macs only, never a request-supplied one) both
     * callers share. */
    plants_adopt_from_registry(&reg_snap, now, interval_s);
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
    /* One extra early sample ~2 minutes after boot: readings reach the
     * live registry within seconds, but the periodic timer's first tick is
     * a full interval out -- on a fresh (or just-rebooted) hub that left
     * History empty for 15 minutes while the Dashboard already showed
     * data, which reads as "history is broken". Two minutes is enough for
     * the BLE scan (and a relaying node's first forward) to have heard
     * the probes; anything unheard by then is skipped by sample_once()'s
     * usual guards and simply waits for the periodic tick. Best-effort:
     * losing the early sample costs nothing but the old wait. */
    const esp_timer_create_args_t t1 = { .callback = timer_cb, .name = "sampler1st" };
    esp_timer_handle_t first;
    if (esp_timer_create(&t1, &first) == ESP_OK) {
        esp_timer_start_once(first, 120ULL * 1000000ULL);
    }
    return esp_timer_start_periodic(timer, (uint64_t)CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60 * 1000000ULL);
}
