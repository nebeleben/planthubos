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

static bool id_in_list(const uint8_t *ids, size_t n, uint8_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

/* Auto-create sweep: a mac the registry already knows but no plant has
 * claimed (a new sensor that appeared mid-uptime) gets a plant now, off the
 * same registry snapshot sample_once() already took. It is sampled starting
 * the NEXT tick, not this one -- the plant loop already ran -- which is
 * fine: the alternative (special-casing a just-created plant into this
 * tick) buys one sample interval of latency at the cost of real complexity.
 *
 * M2 Task 4 hardware hotfix: this used to go through plants_adopt_from_registry()
 * (plants.h, M2-SHIM), which took a SECOND, ~832-byte legacy_registry_t
 * snapshot (data_core_snapshot_legacy()) purely so it could read mac-shaped
 * sensor_entry_t.mac -- a whole extra full-table registry copy that existed
 * only to decode something already sitting in `reg`, undecoded: for a
 * DEV_KIND_BLE device, `id.addr[0..5]` IS the 6-byte mac
 * plants_resolve_or_create() wants (device_id_from_mac() pads 6->8 with
 * zero -- capability.h). That second snapshot was one of the static buffers
 * that pushed the sampler task's own xTaskCreate() into ESP_ERR_NO_MEM right
 * after WiFi/BLE init on a C3 -- see task-4-report.md's "Hardware hotfix"
 * section. Only DEV_KIND_BLE devices are considered, matching
 * legacy_registry_t's own "BLE-kind devices only, others skipped" contract
 * (registry_compat.h) -- ESP-NOW/Zigbee auto-create isn't a V1 concept and
 * isn't introduced here. Liveness-gated exactly like the retired shim call
 * was: a dead device is not re-adopted into a plant (see plants.h's
 * plants_adopt_from_registry() doc comment for the full rationale, still
 * accurate -- this reproduces the same gate, just reading `reg` directly). */
static void adopt_from_registry(const registry_t *reg, uint32_t now_s, uint32_t liveness_s)
{
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        const device_entry_t *e = &reg->devices[i];
        if (!e->in_use || e->id.kind != DEV_KIND_BLE) continue;
        if (now_s - e->last_seen_s > liveness_s) continue;
        plants_resolve_or_create(e->id.addr);
    }
}

static void sample_once(void)
{
    /* only touched on the sampler task. M2 Task 4 hardware hotfix: `reg_snap`
     * is the only full-table registry snapshot needed now (the legacy
     * shim's second copy is gone, see adopt_from_registry() above), and
     * `plant_ids` replaces a full plants_snapshot() (~1953-byte
     * plants_table_t) with just the up-to-PLANTS_MAX id bytes this loop
     * actually needs (plants.h's plants_ids()) -- everything else per plant
     * (bindings, values) is read live, per id, via plants_bindings()/
     * plants_cap_value() below anyway, so the snapshot's mac/name/binding
     * fields were always dead weight here. */
    static registry_t reg_snap;
    static uint8_t plant_ids[PLANTS_MAX];
    data_core_snapshot(&reg_snap);
    size_t n_plants = plants_ids(plant_ids, PLANTS_MAX);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t interval_s = CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60;

    /* Self-healing sweep (M8 Task 6 review fix -- MEDIUM 3b, also closes
     * Task 5's deferred "16-slot cache never frees a claimed slot" note):
     * plant ids are never reused (plants_table.h), so a deleted plant's
     * s_agg slot would otherwise sit claimed forever -- past PLANTS_MAX
     * (16) lifetime create/delete cycles, every slot would be permanently
     * pinned to a dead id and hourly aggregation would silently stop for
     * any newer plant. Free any slot whose plant no longer exists in THIS
     * tick's id list before sampling; bounded PLANTS_MAX x PLANTS_MAX worst
     * case (id_in_list() is a short linear scan over <= PLANTS_MAX ids),
     * still allocation-free, same cost shape as the rest of this per-tick
     * work. */
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (s_agg_used[i] && !id_in_list(plant_ids, n_plants, s_agg_plant_id[i])) {
            s_agg_used[i] = false;
        }
    }

    /* Plants are the unit of sampling (M8 Task 5): walk the plant ids, not
     * the sensor registry. M2 Task 4: what a plant samples is now its
     * capability bindings, not a single assigned probe mac -- a plant with
     * no bindings has nothing to sample and writes nothing, per the M2
     * device-model spec. `bindings` is `static` like reg_snap/plant_ids
     * above: only ever touched on the sampler task, reused fresh every
     * iteration. */
    static plant_binding_t bindings[CAPABILITY_COUNT];
    for (size_t pi = 0; pi < n_plants; pi++) {
        uint8_t plant_id = plant_ids[pi];

        size_t n = plants_bindings(plant_id, bindings, CAPABILITY_COUNT);
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
            if (!plants_cap_value(plant_id, cap_id, &reg_snap, &value, &age_s)) {
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
            int col = storage_col_for(s_base, plant_id, STORAGE_TIER_RAW, cap_id);
            (void)storage_col_for(s_base, plant_id, STORAGE_TIER_HOURLY, cap_id);
            if (col < 0) {
                ESP_LOGW(TAG, "plant %u: history column map full, dropping cap %u", plant_id, cap_id);
                continue;
            }

            int16_t raw = capability_encode(cap_id, value);
            if (raw == CAP_VALUE_NONE) {
                ESP_LOGW(TAG, "plant %u: cap %u value out of range, dropping", plant_id, cap_id);
                continue;
            }
            rec.col[col] = raw;
            any_value = true;
        }
        if (!any_value) continue;   /* every binding stale/unbound this tick: nothing to record */

        if (storage_append(s_base, plant_id, STORAGE_TIER_RAW, &rec) != 0) {
            ESP_LOGW(TAG, "raw append failed for plant %u", plant_id);
            continue;
        }
        hourly_agg_t *agg = agg_for(plant_id);
        storage_rec_t hr;
        if (agg && hourly_agg_add(agg, &rec, &hr)) {
            if (storage_append(s_base, plant_id, STORAGE_TIER_HOURLY, &hr) != 0)
                ESP_LOGW(TAG, "hourly append failed for plant %u", plant_id);
        }
    }

    adopt_from_registry(&reg_snap, now, interval_s);
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

/* Creates the sampler task and its two timers. Split out of sampler_start()
 * (M2 Task 4 hardware hotfix) so it can be retried: `s_wake` is created once
 * by sampler_start() itself and is safe to reuse across attempts (a failed
 * xTaskCreate() never consumes it, and no earlier task exists to have
 * consumed it either). Returns whatever the first step that fails returns;
 * ESP_ERR_NO_MEM from xTaskCreate() specifically is what sampler_start()
 * treats as worth retrying. */
static esp_err_t sampler_bringup(void)
{
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

/* One-shot retry callback (M2 Task 4 hardware hotfix), fired ~15s after a
 * bring-up that failed with ESP_ERR_NO_MEM -- see sampler_start(). By then
 * WiFi/BLE's own init-time allocations have settled, so the same 4096-byte
 * task stack that failed at boot is much more likely to succeed. Logs the
 * outcome either way; a second failure leaves history sampling disabled for
 * the rest of this boot (no further retries -- a heap this consistently
 * short is a standing condition, not a transient boot-time spike, and is
 * better surfaced once than retried forever). */
static void sampler_retry_cb(void *arg)
{
    esp_err_t err = sampler_bringup();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "sampler: retry succeeded; history sampling is now active");
    } else {
        ESP_LOGE(TAG, "sampler: retry failed too (%s); history sampling stays disabled for the "
                      "rest of this boot", esp_err_to_name(err));
    }
}

esp_err_t sampler_start(const char *base_path)
{
    s_base = base_path;
    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) return ESP_ERR_NO_MEM;

    esp_err_t err = sampler_bringup();
    if (err != ESP_ERR_NO_MEM) {
        return err;   /* success, or a non-transient failure not worth retrying */
    }

    /* Boot is the worst possible moment for a 4KB task-stack allocation --
     * WiFi and BLE have both just finished their own init-time allocations
     * -- so a single ESP_ERR_NO_MEM here is very likely transient rather
     * than a real "this device is out of RAM" condition (M2 Task 4 hardware
     * hotfix: this is exactly what was observed on a C3 with M2's registry/
     * plant-table static growth). Caller (main.c) still logs this failure
     * itself, same as always; this schedules the actual recovery. */
    ESP_LOGW(TAG, "sampler: bring-up failed (%s), most likely transient right after WiFi/BLE init "
                  "-- retrying once in 15s instead of disabling history sampling for the rest of "
                  "this boot", esp_err_to_name(err));
    const esp_timer_create_args_t rt = { .callback = sampler_retry_cb, .name = "samplerRetry" };
    esp_timer_handle_t retry_timer;
    esp_err_t rerr = esp_timer_create(&rt, &retry_timer);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "sampler: could not schedule the retry either (%s); history sampling stays "
                      "disabled for the rest of this boot", esp_err_to_name(rerr));
        return err;   /* original ESP_ERR_NO_MEM */
    }
    esp_timer_start_once(retry_timer, 15ULL * 1000000ULL);
    return err;   /* still ESP_ERR_NO_MEM this call -- the scheduled retry above, not this return
                   * value, is what actually recovers history sampling; see sampler_retry_cb(). */
}
