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
#include "esp_system.h"
#include "esp_heap_caps.h"
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

/* M2 Task 4 hardware hotfix, round 2: `s_wake` and the sampler task's own
 * stack/TCB moved to STATIC allocation (StaticSemaphore_t/StackType_t[]/
 * StaticTask_t below), not because .bss is short -- round 1 already bought
 * back ~2769B there -- but because round 1's fix did NOT resolve the C3
 * boot failure (see sampler_bringup()'s doc comment for the hardware
 * evidence and the fragmentation reasoning). xTaskCreate()/
 * xSemaphoreCreateBinary() both pull from the SAME heap region WiFi/BLE
 * init has just been fragmenting; a task needs one contiguous ~4KB+ block,
 * which is exactly the shape most likely to be starved by fragmentation
 * even when total free bytes look healthy (heap_caps_get_largest_free_block()
 * is the metric that matters, not esp_get_free_heap_size() alone). Static
 * allocation draws this memory from .bss at LINK time instead -- a fixed,
 * always-available address, zero dependency on the runtime heap allocator's
 * fragmentation state. CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION is already
 * enabled project-wide (sdkconfig). Stack size kept at the existing 4096
 * bytes, unchanged -- no `uxTaskGetStackHighWaterMark()` measurement is
 * available without hardware access this round, so this deliberately does
 * NOT guess a smaller size; only WHERE the bytes come from changed. */
static StaticSemaphore_t s_wake_buf;
static StackType_t s_sampler_stack[4096];
static StaticTask_t s_sampler_tcb;

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

/* M2 Task 4 hardware hotfix, round 2 -- diagnostics. Round 1 (static-.bss
 * cuts + a single 15s retry) did NOT fix the C3 boot failure: reflashed
 * hardware (commit 91aca6b) still showed `xTaskCreate`-shaped bring-up
 * failing, AND the 15s retry failing again with the identical error --
 * despite round 1 genuinely growing the initial DRAM heap region (48048 ->
 * 50064 bytes, MORE than M1 had, per `heap_init`'s boot log). That
 * "more total heap, still fails, retry doesn't help" combination is the
 * signature of FRAGMENTATION, not exhaustion: `xTaskCreate()` needs one
 * CONTIGUOUS block for the task's stack+TCB, and WiFi/BLE's own init-time
 * allocations (both start before sampler_start(), see main.c's boot order)
 * are well known to leave the heap chopped into many small live/free
 * blocks -- a moment where total-free can look fine while the single
 * largest free block is far smaller. A retry 15s later doesn't help
 * because WiFi/BLE aren't still ALLOCATING at that point, they're just
 * *sitting* on the blocks they already grabbed -- there's nothing to
 * "settle".
 *
 * We don't have hardware access this round (the coordinator reflashes and
 * reports back), so this function logs the exact numbers that distinguish
 * fragmentation from exhaustion -- esp_get_free_heap_size() (total),
 * esp_get_minimum_free_heap_size() (the worst it's ever been, so a boot-log
 * grep doesn't need to catch the failure instant), and
 * heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)
 * (the number that actually gates whether a 4KB+ allocation can succeed) --
 * at every point in sampler_bringup() that can plausibly return
 * ESP_ERR_NO_MEM, tagged with WHICH call failed. Permanent, not a
 * throwaway print: this is the only way to tell "task creation still can't
 * get a big enough block" apart from "one of the two esp_timer_create()
 * calls' much smaller allocation is failing instead" on a future boot,
 * without re-guessing. */
static void log_heap_diag(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "sampler: %s failed (%s) -- heap: free=%u B, min_free_ever=%u B, "
                  "largest_free_block(8BIT|INTERNAL)=%u B",
             what, esp_err_to_name(err),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

/* Creates the sampler task and its two timers. Split out of sampler_start()
 * so it can be retried (see sampler_start()'s own doc comment for why one
 * retry is still kept despite the fixes below).
 *
 * The task itself is now created with xTaskCreateStatic() (s_sampler_stack/
 * s_sampler_tcb, file top) instead of xTaskCreate() -- see those statics'
 * doc comment for the fragmentation reasoning this responds to. With a
 * valid static buffer pair (always true here), xTaskCreateStatic() cannot
 * fail from heap pressure at all; the log_heap_diag() call on that path is
 * defense-in-depth for a future change to this function, not an expected
 * outcome. That leaves the two esp_timer_create() calls below as the only
 * plausible remaining source of an ESP_ERR_NO_MEM from this function --
 * each is a single small (roughly 100-150 byte) allocation, far less
 * likely to be blocked by fragmentation than the 4KB+ contiguous block the
 * task used to need, but not impossible, and now individually diagnosed
 * rather than lumped into one generic "bring-up failed". */
static esp_err_t sampler_bringup(void)
{
    TaskHandle_t task = xTaskCreateStatic(sampler_task, "sampler", sizeof(s_sampler_stack), NULL, 3,
                                          s_sampler_stack, &s_sampler_tcb);
    if (!task) {
        log_heap_diag("xTaskCreateStatic(sampler)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t t = { .callback = timer_cb, .name = "sampler" };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&t, &timer);
    if (err != ESP_OK) {
        log_heap_diag("esp_timer_create(sampler, periodic)", err);
        return err;
    }
    /* One extra early sample ~2 minutes after boot: readings reach the
     * live registry within seconds, but the periodic timer's first tick is
     * a full interval out -- on a fresh (or just-rebooted) hub that left
     * History empty for 15 minutes while the Dashboard already showed
     * data, which reads as "history is broken". Two minutes is enough for
     * the BLE scan (and a relaying node's first forward) to have heard
     * the probes; anything unheard by then is skipped by sample_once()'s
     * usual guards and simply waits for the periodic tick. Best-effort:
     * losing the early sample costs nothing but the old wait -- but still
     * diagnosed on failure (not just silently skipped) since a second
     * small allocation failing right after the first one succeeded would
     * itself be a useful data point. */
    const esp_timer_create_args_t t1 = { .callback = timer_cb, .name = "sampler1st" };
    esp_timer_handle_t first;
    esp_err_t ferr = esp_timer_create(&t1, &first);
    if (ferr == ESP_OK) {
        esp_timer_start_once(first, 120ULL * 1000000ULL);
    } else {
        log_heap_diag("esp_timer_create(sampler1st, one-shot, non-fatal)", ferr);
    }
    return esp_timer_start_periodic(timer, (uint64_t)CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60 * 1000000ULL);
}

/* One-shot retry callback, fired ~15s after a bring-up that failed with
 * ESP_ERR_NO_MEM -- see sampler_start(). Kept from round 1 as a safety net
 * for the two esp_timer_create() calls that remain heap-dependent (the task
 * itself no longer is, see sampler_bringup()'s doc comment) -- a genuine,
 * if less likely, transient heap-exhaustion moment for a ~150-byte
 * allocation is still worth one retry. NOT a fix for fragmentation-driven
 * task-creation failures on its own (round 1's evidence already showed a
 * 15s wait alone doesn't heal fragmentation -- WiFi/BLE aren't still
 * allocating at that point, so there's nothing to "settle"); the static
 * task allocation above is what actually addresses that. Logs the outcome
 * either way; a second failure leaves history sampling disabled for the
 * rest of this boot (no further retries -- see log_heap_diag()'s numbers
 * on that second failure to tell a standing shortage apart from bad luck). */
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
    /* Static, not xSemaphoreCreateBinary() -- see s_wake_buf's doc comment
     * at the top of this file. Only fails on a bad buffer pointer (never,
     * here), so this is defense-in-depth, same as sampler_bringup()'s task
     * check. */
    s_wake = xSemaphoreCreateBinaryStatic(&s_wake_buf);
    if (!s_wake) {
        log_heap_diag("xSemaphoreCreateBinaryStatic(s_wake)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sampler_bringup();
    if (err != ESP_ERR_NO_MEM) {
        return err;   /* success, or a non-transient failure not worth retrying */
    }

    /* See sampler_retry_cb()'s doc comment: this is a safety net for the
     * two esp_timer_create() calls, not the primary fix for the C3
     * fragmentation failure round 1's evidence pointed to (that's the
     * static task allocation above). Caller (main.c) still logs this
     * failure itself, same as always; this schedules the actual recovery
     * attempt. */
    ESP_LOGW(TAG, "sampler: bring-up failed (%s) -- retrying once in 15s (see log_heap_diag() "
                  "output above for which call failed and the heap numbers at that moment)",
             esp_err_to_name(err));
    const esp_timer_create_args_t rt = { .callback = sampler_retry_cb, .name = "samplerRetry" };
    esp_timer_handle_t retry_timer;
    esp_err_t rerr = esp_timer_create(&rt, &retry_timer);
    if (rerr != ESP_OK) {
        log_heap_diag("esp_timer_create(samplerRetry)", rerr);
        ESP_LOGE(TAG, "sampler: could not schedule the retry either; history sampling stays "
                      "disabled for the rest of this boot");
        return err;   /* original ESP_ERR_NO_MEM */
    }
    esp_timer_start_once(retry_timer, 15ULL * 1000000ULL);
    return err;   /* still ESP_ERR_NO_MEM this call -- the scheduled retry above, not this return
                   * value, is what actually recovers history sampling; see sampler_retry_cb(). */
}
