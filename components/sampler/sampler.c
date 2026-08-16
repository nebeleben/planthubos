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
 * NOT guess a smaller size; only WHERE the bytes come from changed.
 *
 * Round 4 KEPT this, having reconsidered it. Round 3's boot heap trace
 * showed the real defect was elsewhere -- sampler_start() was simply being
 * called at the one moment in the boot sequence when only 2624 B of heap
 * remained (main.c now starts it ~79 KB earlier, right after plants_init())
 * -- so the fragmentation reasoning above was not, on its own, the whole
 * story. Reverting to xTaskCreate() would give these 4096+336 bytes back to
 * the heap, but that is a wash in total DRAM (.bss and the heap pool come
 * out of the same 320 KB), and it would re-couple a headline feature's
 * availability to the allocator's state at one instant during a boot
 * sequence that has repeatedly proven tight on this board. A guaranteed
 * start is worth more here than 4.4 KB of pooled-vs-reserved bookkeeping.
 * What round 4 DID add is the measurement round 2 could not make: see
 * sampler_task()'s stack high-water-mark log.
 *
 * ROUND 5 -- why 4096 STAYS, having been asked to cut it to 1536 on the
 * strength of that first watermark reading (3692 B unused of 4096, i.e.
 * 404 B used). It must not be cut, and the watermark is not why:
 *
 * That reading was taken on a tick that wrote nothing (see sampler_task()),
 * so it never entered storage_append(). The number that matters is the
 * worst-case CALL CHAIN, measured on the linked image itself -- every
 * `addi sp,sp,-N` prologue in planthub.elf, walked as a call graph from
 * sampler_task():
 *
 *   sampler_task 160 -> storage_col_for 176 -> open_or_create 48
 *     -> create_fresh 320 -> fopen 32 -> _fopen_r 48 -> _open_r 48
 *     -> vfs_littlefs_open 32 -> lfs_file_opencfg 32 + _ 96
 *     -> lfs_file_close_ 16 -> lfs_file_sync_ 64 -> lfs_dir_commit 16
 *     -> lfs_fs_deorphan 160 -> lfs_dir_orphaningcommit 160
 *     -> lfs_dir_relocatingcommit 144 -> lfs_dir_split 80
 *     -> lfs_dir_compact 128 -> lfs_dir_traverse x2 448      = 2208 B
 *
 * plus the flash tail below that traverse (lfs_dir_getslice 96, lfs_bd_read
 * 64, littlefs_esp_part_read 32, esp_partition_read 48, esp_flash_read 80,
 * HAL/ROM leaf) ~= 420 B, plus the RISC-V exception frame pushed on the
 * task's own stack at interrupt entry (~200 B): **~2.8 KB worst case.**
 *
 * That is the ordinary first-write path, not an exotic one -- create_fresh()
 * runs the first time any plant records to a tier, and the deorphan/split/
 * compact chain is just LittleFS growing a directory. 4096 leaves ~1.3 KB
 * (32%) of margin; 3072 would leave ~270 B; 1536 would overflow by ~1.3 KB
 * and panic. It also matches this project's own standing rule, already
 * written into sdkconfig.defaults for the event-loop task: "IDF recommends
 * >=4KB stack for VFS file I/O". The sampler does VFS file I/O.
 *
 * The ~2.5 KB this would have returned to the heap is also no longer the
 * lever it was: round 4 took free heap after BLE bring-up from 2624 B to a
 * hardware-confirmed 16756 B. Trading a third of a task's stack margin for
 * 15% more heap on a device that now has headroom is the wrong side of that
 * trade. Revisit only with a watermark reading whose ring-file-write count
 * is non-zero. */
static StaticSemaphore_t s_wake_buf;
static StackType_t s_sampler_stack[4096];
static StaticTask_t s_sampler_tcb;

/* Successful raw ring-file appends so far this boot. Not a statistic anyone
 * consumes -- it exists so sampler_task()'s stack high-water-mark line can
 * state whether the deepest path (storage_append -> fopen/fwrite -> LittleFS)
 * has run at all by the time that mark was taken. Round 5 exists because a
 * watermark read WITHOUT that qualifier was mistaken for a worst case. */
static uint32_t s_ring_writes;

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
 * plants.h's plants_adopt_from_registry() does exactly this (same
 * signature, same DEV_KIND_BLE-only/liveness-gated behaviour) -- Task 7
 * moved it off the M2 registry-compat shim onto a real registry_t*, so
 * sample_once() below just calls it directly on `reg_snap` instead of
 * keeping a second, duplicate copy of this loop (M2 Task 4's hardware
 * hotfix note that used to live here -- a SECOND, ~832-byte legacy_registry_t
 * snapshot the shim needed just to read mac-shaped sensor_entry_t.mac -- no
 * longer applies now that the shim is gone; see task-7-report.md). */

static void sample_once(void)
{
    /* only touched on the sampler task. `reg_snap` is the only full-table
     * registry snapshot needed here, and `plant_ids` replaces a full
     * plants_snapshot() (~1953-byte plants_table_t) with just the
     * up-to-PLANTS_MAX id bytes this loop actually needs (plants.h's
     * plants_ids()) -- everything else per plant (bindings, values) is read
     * live, per id, via plants_bindings()/plants_cap_value() below anyway,
     * so the snapshot's mac/name/binding fields were always dead weight
     * here. */
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
        /* Counted purely so the stack high-water-mark line below can say
         * whether the deep path has actually run yet -- see sampler_task(). */
        s_ring_writes++;
        hourly_agg_t *agg = agg_for(plant_id);
        storage_rec_t hr;
        if (agg && hourly_agg_add(agg, &rec, &hr)) {
            if (storage_append(s_base, plant_id, STORAGE_TIER_HOURLY, &hr) != 0)
                ESP_LOGW(TAG, "hourly append failed for plant %u", plant_id);
        }
    }

    plants_adopt_from_registry(&reg_snap, now, interval_s);
}

static void sampler_task(void *arg)
{
    /* Round 4 added this mark; round 5 fixed how it is reported, after the
     * first reading was very nearly acted on as if it were a worst case.
     *
     * That reading was `3692 B unused of 4096 B` -- i.e. 404 B used -- taken
     * on a hub whose plants had no bound capabilities yet. sample_once()
     * `continue`s past every plant with no bindings and past every plant
     * whose bindings produced no fresh value, BEFORE it ever reaches
     * storage_col_for()/storage_append(). So that mark measured a tick that
     * opened no file at all: a floor, not a bound. Sizing the stack from it
     * (1536 B was proposed) would have overflowed it by ~1 KB the first time
     * a real plant recorded a sample.
     *
     * Two consequences, both implemented here:
     *
     *   1. The mark is now NEW-LOW-triggered, not once-after-the-first-sample
     *      (the same idiom rules_engine.c's engine_task uses, and for the same
     *      reason): the deep pass is rare and data-dependent, so "the first
     *      one" is exactly the wrong sample to trust. This line will re-fire
     *      the first time a genuine ring-file write goes deeper.
     *   2. It reports s_ring_writes alongside the mark. A reading with
     *      `after 0 ring-file writes` is explicitly NOT a worst case and must
     *      not be used to resize s_sampler_stack. Wait for a non-zero count.
     *
     * For why 4096 stays until then, see s_sampler_stack's own comment. */
    static UBaseType_t lowest_free = (UBaseType_t)-1;
    while (1) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        sample_once();
        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
        if (free_bytes < lowest_free) {
            lowest_free = free_bytes;
            ESP_LOGI(TAG, "sampler task stack high-water mark: %u B unused of %u B "
                          "(after %u ring-file writes -- a mark taken after 0 writes has NOT "
                          "exercised the LittleFS path and must not be used to resize the stack)",
                     (unsigned)free_bytes, (unsigned)sizeof(s_sampler_stack),
                     (unsigned)s_ring_writes);
        }
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

/* Every resource sampler_bringup() creates, tracked so bring-up can be
 * RE-ENTERED safely. See sampler_bringup()'s own comment for why that
 * matters -- these four statics are the mechanism that makes the retry
 * safe, not bookkeeping. All zero/NULL at boot (.bss). */
static TaskHandle_t       s_task;             /* non-NULL once the task exists -- never create twice */
static esp_timer_handle_t s_periodic_timer;   /* non-NULL once created */
static esp_timer_handle_t s_first_timer;      /* non-NULL once created (best-effort resource) */
static bool               s_periodic_running; /* true once esp_timer_start_periodic has succeeded */

/* Brings up the sampler task and its two timers. Split out of
 * sampler_start() so it can be retried (see sampler_start()'s own doc
 * comment for why one retry is still kept despite the fixes below).
 *
 * IDEMPOTENT PER RESOURCE -- this is a correctness guarantee, not a style
 * preference, and it is why the function reads as four independent `if
 * (!already)` blocks rather than a straight-line sequence.
 *
 * Code review (round 6) found the previous all-or-nothing shape could
 * corrupt a FreeRTOS list. The old function created the task
 * unconditionally at its top, and then, if the FOLLOWING
 * esp_timer_create() failed with ESP_ERR_NO_MEM, returned that error --
 * with the task already created and running. sampler_start() treats
 * ESP_ERR_NO_MEM as retryable and schedules sampler_retry_cb() 15 s later,
 * which called this function again, re-running xTaskCreateStatic() on the
 * SAME s_sampler_tcb/s_sampler_stack while the first task was alive and
 * linked into a kernel list. Re-initialising a live TCB's embedded list
 * items in place corrupts whichever ready/delayed/event list that task is
 * currently on -- an arbitrary-behaviour bug, not a benign double-create.
 * Static allocation is what makes it dangerous: xTaskCreate() would have
 * returned a second, independent TCB (merely wasteful); xTaskCreateStatic()
 * scribbles over the live one.
 *
 * Low probability today -- after the round-4 reorder there is ~81 KB free
 * at this point and the timer allocation is ~150 B against a task creation
 * that cannot fail at all -- but reachable, and precisely the kind of thing
 * that surfaces on a future board with less headroom. The guarantee now
 * belongs to the code: a retry can only ever attempt the resources that are
 * still missing, so no resource here can be created twice however many
 * times this function is called.
 *
 * On the task itself: xTaskCreateStatic() (s_sampler_stack/s_sampler_tcb,
 * file top) instead of xTaskCreate() -- see those statics' doc comment for
 * the fragmentation reasoning this responds to. With a valid static buffer
 * pair (always true here) it cannot fail from heap pressure at all; the
 * log_heap_diag() call on that path is defense-in-depth for a future change
 * to this function, not an expected outcome. That leaves the two
 * esp_timer_create() calls below as the only plausible source of an
 * ESP_ERR_NO_MEM from this function -- each a single ~100-150 byte
 * allocation, individually diagnosed rather than lumped into one generic
 * "bring-up failed". */
static esp_err_t sampler_bringup(void)
{
    if (!s_task) {
        s_task = xTaskCreateStatic(sampler_task, "sampler", sizeof(s_sampler_stack), NULL, 3,
                                   s_sampler_stack, &s_sampler_tcb);
        if (!s_task) {
            log_heap_diag("xTaskCreateStatic(sampler)", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_periodic_timer) {
        const esp_timer_create_args_t t = { .callback = timer_cb, .name = "sampler" };
        /* Into a local, then published on success only: esp_timer_create()
         * leaves *out_handle untouched on failure, so assigning it directly
         * would risk leaving s_periodic_timer holding whatever it held
         * before (here: NULL, correct by luck) -- this is correct by
         * construction instead, which is what the retry path needs. */
        esp_timer_handle_t timer = NULL;
        esp_err_t err = esp_timer_create(&t, &timer);
        if (err != ESP_OK) {
            log_heap_diag("esp_timer_create(sampler, periodic)", err);
            return err;
        }
        s_periodic_timer = timer;
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
     * itself be a useful data point. Never fails bring-up, so a retry that
     * gets this far with everything else already up still returns ESP_OK. */
    if (!s_first_timer) {
        const esp_timer_create_args_t t1 = { .callback = timer_cb, .name = "sampler1st" };
        esp_timer_handle_t first = NULL;
        esp_err_t ferr = esp_timer_create(&t1, &first);
        if (ferr == ESP_OK) {
            s_first_timer = first;
            esp_timer_start_once(s_first_timer, 120ULL * 1000000ULL);
        } else {
            log_heap_diag("esp_timer_create(sampler1st, one-shot, non-fatal)", ferr);
        }
    }

    /* Guarded like the rest: esp_timer_start_periodic() on an
     * already-running timer returns ESP_ERR_INVALID_STATE, which a retry
     * would otherwise report as a bring-up failure even though sampling is
     * live. */
    if (!s_periodic_running) {
        esp_err_t err = esp_timer_start_periodic(s_periodic_timer,
                                                 (uint64_t)CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60 * 1000000ULL);
        if (err != ESP_OK) {
            log_heap_diag("esp_timer_start_periodic(sampler)", err);
            return err;
        }
        s_periodic_running = true;
    }
    return ESP_OK;
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
 * on that second failure to tell a standing shortage apart from bad luck).
 *
 * Round 6: this callback is safe to run against a PARTIALLY brought-up
 * sampler -- which is the normal case that reaches here, since the only
 * plausible failure is a timer allocation AFTER the task already exists.
 * sampler_bringup() is idempotent per resource (see there); it will not
 * re-create the task, the semaphore, or any timer that already exists.
 * Before that fix, this call is what turned a failed 150-byte allocation
 * into a second xTaskCreateStatic() over a live TCB. */
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
     * here), so the NULL check is defense-in-depth, same as
     * sampler_bringup()'s task check.
     *
     * `if (!s_wake)` for the same reason sampler_bringup() guards every one
     * of its resources (round 6): re-running xSemaphoreCreateBinaryStatic()
     * on s_wake_buf would re-initialise a semaphore the live sampler task
     * may be blocked on, dropping it off the queue's waiting-task list.
     * main.c calls sampler_start() exactly once today, so this is
     * unreachable -- but "unreachable today" is exactly what the retry path
     * was assumed to be before review found it, and a guard costs nothing. */
    if (!s_wake) {
        s_wake = xSemaphoreCreateBinaryStatic(&s_wake_buf);
        if (!s_wake) {
            log_heap_diag("xSemaphoreCreateBinaryStatic(s_wake)", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
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
