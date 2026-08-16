#include "sampler.h"
#include "data_core.h"
#include "storage.h"
#include "capability.h"
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

static void sample_once(void)
{
    /* only touched on the sampler task */
    static registry_t reg_snap;             /* V2: drives the binding-mapped sampling loop below */
    static legacy_registry_t reg_snap_legacy;   /* M2-SHIM: only for the auto-create sweep at the end */
    static plants_table_t plant_snap;
    data_core_snapshot(&reg_snap);
    data_core_snapshot_legacy(&reg_snap_legacy);   /* M2-SHIM */
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
     * the sensor registry. M2 Task 4: what a plant samples is now its
     * capability bindings, not a single assigned probe mac -- a plant with
     * no bindings has nothing to sample and writes nothing, per the M2
     * device-model spec. `bindings` is `static` like reg_snap/plant_snap
     * above: only ever touched on the sampler task, reused fresh every
     * iteration. */
    static plant_binding_t bindings[CAPABILITY_COUNT];
    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *p = &plant_snap.p[i];
        if (!p->in_use) continue;

        size_t n = plants_bindings(p->id, bindings, CAPABILITY_COUNT);
        if (n == 0) continue;   /* no bindings: writes nothing */

        storage_rec_t rec;
        rec.boot_id = timekeeper_boot_id();
        rec.rel_s = now;
        for (int c = 0; c < HISTORY_COLS; c++) rec.col[c] = CAP_VALUE_NONE;

        bool any_value = false;
        for (size_t b = 0; b < n; b++) {
            uint8_t cap_id = bindings[b].cap_id;
            float value;
            uint32_t age_s;
            if (!plants_cap_value(p->id, cap_id, &reg_snap, &value, &age_s)) {
                /* Unbound-in-practice (device not in this snapshot) or no
                 * value in its slot yet -- e.g. a probe just bound but not
                 * heard from this boot. Not a warning: routine right after
                 * a fresh binding or a reboot. */
                continue;
            }
            /* skip a capability that produced nothing since the last sample
             * (dead battery / gone) -- per-capability now, not per-device,
             * since a plant's bindings can span multiple physical devices. */
            if (age_s > interval_s) continue;

            /* Ensure the column on BOTH tiers (see storage_compat.h's old
             * shim doing the same): hourly_agg_add() below builds the
             * hourly record straight from `rec`'s column layout, so the two
             * tiers must already agree on what each column means before
             * that happens. */
            int col = storage_col_for(s_base, p->id, STORAGE_TIER_RAW, cap_id);
            (void)storage_col_for(s_base, p->id, STORAGE_TIER_HOURLY, cap_id);
            if (col < 0) {
                ESP_LOGW(TAG, "plant %u: history column map full, dropping cap %u", p->id, cap_id);
                continue;
            }

            int16_t raw = capability_encode(cap_id, value);
            if (raw == CAP_VALUE_NONE) {
                ESP_LOGW(TAG, "plant %u: cap %u value out of range, dropping", p->id, cap_id);
                continue;
            }
            rec.col[col] = raw;
            any_value = true;
        }
        if (!any_value) continue;   /* every binding stale/unbound this tick: nothing to record */

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
     * callers share. Still mac-based (M2-SHIM) -- auto-CREATING a plant for
     * a newly-seen probe is unrelated to what capabilities it later gets
     * bound to, and is not rewired by this task (RULING-1). */
    plants_adopt_from_registry(&reg_snap_legacy, now, interval_s);   /* M2-SHIM */
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
