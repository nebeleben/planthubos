#include "plants.h"
#include "plants_migrate.h"
#include "capability.h"
#include "storage.h"
#include "storage_compat.h"   /* M2-SHIM */
#include "timekeeper.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "plants";
#define NS "planthub"
#define KEY_PLANTS "plants"
#define PLANTS_FILE_NAME "plants.bin"
#define PLANTS_TMP_NAME  "plants.tmp"

/* ---------------- Why this lives on LittleFS, not NVS (M8 hardware bring-up) ----------------
 *
 * plants.c used to persist this same blob to NVS, namespace "planthub", key
 * "plants" -- the same namespace claim/wifi/swarm state lives in. Every
 * mutation (rename, assign, create, delete, auto-create) wrote the full
 * 674-byte blob, and NVS never overwrites a value in place: each write lands
 * a fresh copy and the old one becomes garbage, reclaimed later by page GC.
 * On actual hardware this produced 19 stale blob copies in the 24KB NVS
 * partition within a single session -- a plant table that gets renamed/
 * assigned/probed into existence is not a rare-write structure, unlike the
 * claim/wifi/swarm state the partition was sized around -- forcing constant
 * GC in the SAME partition that holds that other state. A crash mid-GC
 * coincided with the hub's swarm role/node-table state reverting: NVS GC is
 * an all-or-nothing sweep over the whole partition, so a plant-driven GC
 * storm put swarm's own rarely-written state at risk of the same crash
 * window, even though swarm never touches NVS anywhere near that often
 * itself. Moving the frequent writer to its own file removes that blast
 * radius entirely: LittleFS's per-file writes cannot trigger NVS GC, no
 * matter how often plants.bin gets rewritten. See persist_table()/
 * write_file() below for the new steady-state path, and plants_init() for
 * the one-boot migration that adopts an existing M8-era NVS blob (nvs.h
 * stays included for exactly that one-time path -- nothing else here
 * touches NVS any more). */

/* On-disk format of the plants.bin blob (identical layout to the old NVS
 * blob this replaces, including the legacy KEY_PLANTS blob the one-boot
 * migration in plants_init() reads). Bump this and add a migration branch
 * in load_file()/load_legacy_nvs_blob() below whenever plant_entry_t's shape
 * changes -- same reasoning as swarm_store.c's SWARM_STORE_FORMAT for the
 * node table. Deliberately NOT plants_table_t dumped raw: that struct's
 * `bool` fields and any compiler-inserted padding are not a stable on-disk
 * shape, so this mirror struct pins every field to an explicit width/order
 * instead.
 *
 * Format 2 (M2 Task 4): adds each plant's capability bindings (cap_bound[]/
 * cap_dev[], plants_table.h). No migration branch for format 1 -- per the
 * M2 device-model plan, this is a clean-start format change: a format-1
 * blob is already the wrong LENGTH (674B vs this format's 1954B, see
 * plant_entry_blob_t below) and gets discarded by load_file()'s existing
 * "unrecognised size" branch before the format byte is even checked, so
 * bumping this number is defense in depth, not the primary guard. An
 * upgrading hub starts with an empty plant table (logged) rather than a
 * silently-reinterpreted one -- see task-4-report.md. */
#define PLANTS_BLOB_FORMAT 2

typedef struct __attribute__((packed)) {
    uint8_t id;
    uint8_t in_use;      /* bool, packed as u8 */
    uint8_t mac[6];
    uint8_t mac_valid;   /* bool, packed as u8 */
    char    name[PLANT_NAME_LEN + 1];
    uint8_t     cap_bound[CAPABILITY_COUNT];   /* bool, packed as u8, indexed by cap_id */
    device_id_t cap_dev[CAPABILITY_COUNT];     /* device_id_t is all-uint8_t: safe to embed directly */
} plant_entry_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t           format;
    uint8_t           next_id;
    plant_entry_blob_t p[PLANTS_MAX];
} plants_blob_t;

/* ---------------- Locking invariant ----------------
 *
 * Same two-phase discipline as swarm_store.c (see its own comment block for
 * the full derivation) applied to a single table instead of several scalars
 * -- read that comment first if you're touching this. Short version:
 *
 *   - s_mutex guards ONLY the in-RAM s_table -- every hold is a short,
 *     bounded, allocation-free struct copy/mutation, never file I/O, and
 *     never held while also contending for s_persist_mutex.
 *     plants_snapshot() and every other reader can therefore never be made
 *     to wait on a file write, no matter what else is happening
 *     concurrently.
 *
 *   - s_persist_mutex guards the actual plants.bin write (write_file()'s
 *     tmp-then-rename), performed by persist_table() below. Every mutating
 *     call (plants_resolve_or_create, plants_rename, plants_assign,
 *     plants_create, plants_delete) follows the same shape: PHASE 1 (RAM,
 *     under s_mutex only) mutates s_table and releases s_mutex completely;
 *     PHASE 2 (persist_table()) takes s_persist_mutex, briefly RE-takes
 *     s_mutex just long enough to copy the CURRENT s_table -- not whatever
 *     phase 1 computed, which may already be stale by the time phase 2
 *     actually runs -- releases s_mutex, writes the blob, then releases
 *     s_persist_mutex. This closes the same two gaps swarm_store.c's
 *     comment documents: a reader can never transitively wait on another
 *     writer's in-flight write, and two concurrent writers' writes can
 *     never land out of order relative to RAM freshness (whichever one
 *     actually holds s_persist_mutex at write time always writes the
 *     freshest RAM state, so the file only ever moves forward).
 *
 *   - Consequence, same as swarm_store.c: the RAM cache can run ahead of
 *     the file if a write fails (logged, never silently swallowed) -- it
 *     remains the source of truth for the rest of THIS boot regardless. A
 *     reboot before a failed write's next retry reverts to whatever last
 *     actually landed on disk.
 *
 * plants_resolve_or_create() in particular is documented as TASK CONTEXT
 * ONLY (plants.h) -- unlike swarm_store's node lookups it is never called
 * from an ISR or a must-not-block callback, but it still sits on the
 * sampler/BLE ingestion path, so keeping its file write off the RAM mutex
 * (via the same two-phase split) still matters: a concurrent HTTP-triggered
 * rename's file write must never stall the next sensor sample's mac
 * resolution. */
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_persist_mutex;
static plants_table_t s_table;
static const char *s_storage_base;   /* "/storage", or NULL: see plants_init() */

/* Boot-time-only scratch, shared by load_file() and load_legacy_nvs_blob()
 * (M2 Task 4 review): both read one plants_blob_t off disk/NVS, once,
 * synchronously, strictly sequentially, all on the main task during
 * plants_init() -- load_file() runs first and is done with its blob (either
 * unpacked into s_table or discarded on failure) before
 * plants_adopt_legacy_nvs() -> load_legacy_nvs_blob() ever runs, so there is
 * no window where both need their own copy live at once. Sharing this one
 * ~1954-byte static in place of two separate ones (each function's own doc
 * comment used to justify its own copy on the same "post-mortem
 * stack-safety fix" grounds) removes one buffer's worth of that history's
 * growth -- see task-4-report.md's "Fix round" section. Do NOT reuse this
 * for anything reachable outside plants_init()'s one-boot load sequence. */
static plants_blob_t s_boot_blob_scratch;

/* plants_last_values() RAM cache -- guarded by its own mutex, deliberately
 * separate from s_mutex/s_persist_mutex above: it is populated by a
 * storage_query() file scan (real I/O), which must never run while holding
 * a lock that plants_snapshot()/plants_resolve_or_create() etc. might be
 * waiting on. One slot per currently-existing plant is always enough
 * (PLANTS_MAX), because plants_delete() below drops a plant's cache slot
 * (see lv_cache_drop()) in the same call that frees its plants_table.h
 * slot for a new plant -- a deleted plant can never hold a cache slot
 * hostage. */
#define LAST_VALUES_CACHE_CAP PLANTS_MAX
typedef struct {
    bool     used;
    uint8_t  id;
    bool     found;             /* false = queried once, plant had no history */
    int16_t  temp_dc;
    uint8_t  moisture_pct;
    uint32_t lux;
    uint16_t conductivity_us;
    uint8_t  battery_pct;
    uint32_t epoch;
} last_values_cache_t;
static SemaphoreHandle_t s_lv_mutex;
static last_values_cache_t s_last_values[LAST_VALUES_CACHE_CAP];

/* "plant table full" is logged at most once per distinct mac per boot,
 * mirroring battery_sched.h's small fixed-size in_use table -- otherwise a
 * single misbehaving/looping sensor hammering plants_resolve_or_create()
 * against a full table would spam the log forever. PLANTS_MAX (16) slots is
 * already generous: there can never be more than PLANTS_MAX distinct macs
 * actively colliding with a full table at once in practice. If this cache
 * itself fills up (16 distinct never-before-seen macs logged this boot
 * already), later ones just log every time -- graceful degradation, not a
 * correctness issue. */
#define FULL_LOG_CAP 16
typedef struct {
    bool    used;
    uint8_t mac[6];
} full_logged_t;
static full_logged_t s_full_logged[FULL_LOG_CAP];

static void log_full_once(const uint8_t mac[6])
{
    int free_slot = -1;
    for (int i = 0; i < FULL_LOG_CAP; i++) {
        if (s_full_logged[i].used) {
            if (memcmp(s_full_logged[i].mac, mac, 6) == 0) return;  /* already logged this boot */
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    ESP_LOGW(TAG, "plant table full (%02X:%02X:%02X:%02X:%02X:%02X): auto-create dropped",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (free_slot >= 0) {
        s_full_logged[free_slot].used = true;
        memcpy(s_full_logged[free_slot].mac, mac, 6);
    }
}

/* Packs a live plants_table_t into the on-disk mirror shape (shared by
 * persist_table() below), and the reverse for load_file()/
 * load_legacy_nvs_blob() -- both blob sources (plants.bin and the legacy NVS
 * key) use the exact same PLANTS_BLOB_FORMAT layout, so this pair is shared
 * between them rather than duplicated. */
static void pack_blob(const plants_table_t *in, plants_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->format = PLANTS_BLOB_FORMAT;
    out->next_id = in->next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        out->p[i].id = in->p[i].id;
        out->p[i].in_use = in->p[i].in_use ? 1 : 0;
        memcpy(out->p[i].mac, in->p[i].mac, 6);
        out->p[i].mac_valid = in->p[i].mac_valid ? 1 : 0;
        memcpy(out->p[i].name, in->p[i].name, sizeof(out->p[i].name));
        for (int c = 0; c < CAPABILITY_COUNT; c++) {
            out->p[i].cap_bound[c] = in->p[i].cap_bound[c] ? 1 : 0;
            out->p[i].cap_dev[c] = in->p[i].cap_dev[c];
        }
    }
}

/* Unpacks straight into *out -- deliberately NOT via an intermediate local
 * plants_table_t (a stack-overflow post-mortem fix: that local used to add
 * a second ~673-byte frame stacked directly on top of the caller's own
 * ~674-byte plants_blob_t local, and this function's two callers are both
 * on the plants_init() hot path where a hardware crash-loop traced a "Stack
 * protection fault" in this exact call chain, on the main task, right after
 * a plants.bin-missing WARN -- see plants_init()'s top comment and this
 * file's top-of-file comment for the full story). *out is always the
 * caller's real destination (s_table, or an init-only static -- see
 * load_file()/load_legacy_nvs_blob() below), never a value that needs to
 * survive being overwritten mid-loop, so writing straight through has no
 * correctness cost, only a stack-safety benefit. */
static void unpack_blob(const plants_blob_t *blob, plants_table_t *out)
{
    out->next_id = blob->next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        out->p[i].in_use = blob->p[i].in_use != 0;
        out->p[i].id = blob->p[i].id;
        out->p[i].mac_valid = blob->p[i].mac_valid != 0;
        memcpy(out->p[i].mac, blob->p[i].mac, 6);
        memcpy(out->p[i].name, blob->p[i].name, sizeof(out->p[i].name));
        out->p[i].name[PLANT_NAME_LEN] = '\0';  /* defensive: guard against a corrupt non-terminated blob */
        for (int c = 0; c < CAPABILITY_COUNT; c++) {
            out->p[i].cap_bound[c] = blob->p[i].cap_bound[c] != 0;
            out->p[i].cap_dev[c] = blob->p[i].cap_dev[c];
        }
    }
}

/* Atomic write of one plants.bin: the full blob goes to a sibling .tmp file
 * first, which is fflush()ed and fclose()d before rename()ing it over the
 * real path. rename() is atomic on LittleFS (and on host filesystems, for
 * the test harness), so a reader -- or a power loss -- only ever sees the
 * old complete file or the new complete one, never a partial write. Mirrors
 * boottab.c's write_all(); see that file's comment for the same rationale
 * in more detail. Caller (persist_table()) guarantees s_storage_base is
 * non-NULL.
 *
 * path/tmp_path are `static`, not stack locals (post-mortem stack-safety
 * fix -- see persist_table()'s comment): write_file() is only ever called
 * from persist_table() while it holds s_persist_mutex for that function's
 * ENTIRE body, so exactly one task is ever inside this function at a time
 * no matter which of plants_resolve_or_create()/plants_rename()/
 * plants_assign()/plants_create()/plants_delete() called it in -- including
 * the sampler task, which only has a 4096-byte stack to begin with. Do not
 * call write_file() from anywhere else without re-establishing that same
 * guarantee first. */
static esp_err_t write_file(const plants_blob_t *blob)
{
    static char path[128];
    static char tmp_path[128];
    int n1 = snprintf(path, sizeof(path), "%s/%s", s_storage_base, PLANTS_FILE_NAME);
    int n2 = snprintf(tmp_path, sizeof(tmp_path), "%s/%s", s_storage_base, PLANTS_TMP_NAME);
    if (n1 < 0 || (size_t)n1 >= sizeof(path) || n2 < 0 || (size_t)n2 >= sizeof(tmp_path)) {
        ESP_LOGE(TAG, "plants: storage_base path too long for plants.bin");
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "plants: failed to open %s for writing: %s", tmp_path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(blob, 1, sizeof(*blob), f);
    if (written != sizeof(*blob)) {
        ESP_LOGE(TAG, "plants: short write to %s (%d of %d bytes)",
                 tmp_path, (int)written, (int)sizeof(*blob));
        fclose(f);
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "plants: fflush(%s) failed: %s", tmp_path, strerror(errno));
        fclose(f);
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "plants: fclose(%s) failed: %s", tmp_path, strerror(errno));
        remove(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "plants: rename %s -> %s failed: %s", tmp_path, path, strerror(errno));
        remove(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Phase-2 persistence (see the locking invariant comment above): serialises
 * against other writers via s_persist_mutex, packs the CURRENT s_table into
 * the on-disk mirror shape under a brief s_mutex hold, then writes it (the
 * file write itself happens AFTER s_mutex is released -- see below). Callers
 * must have already completed their own RAM mutation under s_mutex and
 * released it BEFORE calling this -- it must never be called while still
 * holding s_mutex.
 *
 * storage_base == NULL (no filesystem -- see plants_init()) makes this a
 * no-op returning ESP_OK: plants are hub-only and the hub always has
 * storage in practice, so this is only reachable if littlefs itself failed
 * to mount, already logged once at plants_init() time; there is nowhere to
 * write, and the RAM table remains the only copy for the rest of this
 * boot.
 *
 * `blob` is `static`, not a stack local -- post-mortem stack-safety fix:
 * s_persist_mutex is held for this function's ENTIRE body, so exactly one
 * task is ever inside this critical section at a time, which is what makes
 * a ~1954-byte static safe despite persist_table() being reachable from
 * FOUR different task contexts (plants_resolve_or_create() off the
 * sampler/BLE ingestion path, plants_rename()/plants_assign()/
 * plants_create()/plants_delete() off the httpd task, plus MQTT-adjacent
 * callers). Before the original fix this was a plain stack local in a
 * function reachable from the sampler task's 4096-byte stack under normal
 * sampling load -- almost certainly a contributing cause of the ORIGINAL M8
 * hardware incident's crash (this file's top comment), independent of the
 * NVS-GC blast radius that motivated moving off NVS in the first place.
 * Safe to keep as `static` specifically BECAUSE persist_table() already
 * serialises every caller through s_persist_mutex for unrelated reasons
 * (closing the same two write-ordering gaps swarm_store.c's own comment
 * documents) -- if that mutex's scope ever narrows to exclude part of this
 * function, this must move back off `static` storage or gain its own lock.
 *
 * M2 Task 4 review: this used to copy s_table into a second static
 * `snapshot` under s_mutex, release s_mutex, THEN call pack_blob() on the
 * copy -- solely so pack_blob() itself never ran while holding s_mutex.
 * That copy is unnecessary: pack_blob() is a fixed O(PLANTS_MAX),
 * allocation-free, no-I/O loop (see its own doc comment), exactly the kind
 * of work the locking-invariant comment above already permits doing while
 * s_mutex is held ("every hold is a short, bounded, allocation-free struct
 * copy/mutation, never file I/O"). Packing s_table directly removes one
 * ~1953-byte static (see task-4-report.md's "Fix round" section for the
 * measured before/after) without changing what's protected: s_mutex still
 * covers every touch of s_table, and the actual file write (write_file())
 * still happens with s_mutex released, same as before. */
static esp_err_t persist_table(void)
{
    if (!s_storage_base) return ESP_OK;

    xSemaphoreTake(s_persist_mutex, portMAX_DELAY);

    static plants_blob_t blob;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pack_blob(&s_table, &blob);
    xSemaphoreGive(s_mutex);

    esp_err_t err = write_file(&blob);
    xSemaphoreGive(s_persist_mutex);
    return err;
}

/* Reads <storage_base>/plants.bin into *out, which the caller has already
 * reset to plants_table_init() defaults -- every return path below
 * (including every failure) therefore leaves *out valid. A missing file,
 * wrong size, or an unrecognised format byte is loudly logged and left at
 * those defaults (empty table) rather than trusting a shape that cannot be
 * relied on -- never fails boot. Returns true iff a valid blob was loaded.
 * Caller (plants_init()) guarantees s_storage_base is non-NULL.
 *
 * path is `static`, not a stack local (post-mortem stack-safety fix -- see
 * plants_init()'s top comment): load_file() only ever runs once,
 * synchronously, from plants_init() on the main task, and has no other
 * caller -- there is no concurrency to protect against here, only stack
 * depth. This exact call chain (plants_init() -> load_file(), immediately
 * followed on a miss by plants_adopt_legacy_nvs() -> load_legacy_nvs_blob())
 * is where a hardware crash-loop hit a "Stack protection fault" on the main
 * task, right after this function's own "not readable" WARN below. The blob
 * itself is read into s_boot_blob_scratch (M2 Task 4 review: shared with
 * load_legacy_nvs_blob(), see that static's own doc comment for why it's
 * safe for the two to share one buffer) rather than a function-local
 * static. */
static bool load_file(plants_table_t *out)
{
    static char path[128];
    int n = snprintf(path, sizeof(path), "%s/%s", s_storage_base, PLANTS_FILE_NAME);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "plants: %s not readable (%s); starting with an empty plant table",
                 path, strerror(errno));
        return false;
    }

    plants_blob_t *blob = &s_boot_blob_scratch;
    size_t rlen = fread(blob, 1, sizeof(*blob), f);
    int extra = fgetc(f);   /* confirm the file isn't LONGER than expected, too */
    fclose(f);

    if (rlen != sizeof(*blob) || extra != EOF) {
        ESP_LOGW(TAG, "plants: %s has unrecognised size (read %d bytes, expected %d); starting "
                      "with an empty plant table", path, (int)rlen, (int)sizeof(*blob));
        return false;
    }
    if (blob->format != PLANTS_BLOB_FORMAT) {
        ESP_LOGE(TAG, "plants: %s has unknown format byte %u (expected %u) -- DISCARDING it "
                      "rather than trusting an unrecognised layout; every plant is lost until "
                      "re-created", path, blob->format, (unsigned)PLANTS_BLOB_FORMAT);
        return false;
    }

    unpack_blob(blob, out);
    return true;
}

/* ---------------- Legacy NVS blob (one-boot migration only) ----------------
 *
 * Reads the OLD KEY_PLANTS blob straight out of NVS -- the exact bytes an
 * M8-era build last wrote there before this fix (see this file's top
 * comment for the 19-stale-copies story behind moving off NVS). Called
 * ONLY from plants_init()'s one-boot migration below, and only when
 * plants.bin does not exist yet: a live rig upgrading past this fix still
 * has its plant table sitting in NVS from its last boot, and that table
 * must not be silently dropped. Steady state never calls this again --
 * plants no longer writes NVS once plants.bin exists. Same
 * reset-to-defaults-on-any-failure contract as load_file() above.
 *
 * Reads into s_boot_blob_scratch, NOT a function-local static (M2 Task 4
 * review) -- shared with load_file() above. Safe because plants_init()'s
 * boot sequence only ever reaches this function AFTER load_file() has
 * already returned (see plants_init() below: this is only called when
 * load_file() returned false), so load_file()'s own use of the scratch
 * buffer is finished -- unpacked into *out or discarded -- before this
 * function's first write to it. Both calls are synchronous, on the main
 * task, one-boot-only, with nothing else touching the scratch buffer in
 * between. Returns true iff a valid blob was loaded. */
static bool load_legacy_nvs_blob(nvs_handle_t h, plants_table_t *out)
{
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, KEY_PLANTS, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;  /* no legacy blob: never an M8-era device, or already migrated */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "plants: legacy NVS blob length query failed: %s", esp_err_to_name(err));
        return false;
    }
    if (len != sizeof(plants_blob_t)) {
        ESP_LOGW(TAG, "plants: legacy NVS blob has unrecognised length %d (expected %d)",
                 (int)len, (int)sizeof(plants_blob_t));
        return false;
    }

    plants_blob_t *blob = &s_boot_blob_scratch;
    size_t rlen = sizeof(*blob);
    if (nvs_get_blob(h, KEY_PLANTS, blob, &rlen) != ESP_OK || rlen != sizeof(*blob)) {
        ESP_LOGW(TAG, "plants: legacy NVS blob read failed at its expected length");
        return false;
    }
    if (blob->format != PLANTS_BLOB_FORMAT) {
        ESP_LOGE(TAG, "plants: legacy NVS blob has unknown format byte %u (expected %u) -- "
                      "DISCARDING it rather than trusting an unrecognised layout",
                 blob->format, (unsigned)PLANTS_BLOB_FORMAT);
        return false;
    }

    unpack_blob(blob, out);
    return true;
}

/* ---------------- One-boot migration (M8 Task 4) ----------------
 *
 * Executor half of plants_migrate.h's pure planner: discovers what the
 * pre-M8 sensor-keyed world left lying around, hands it to
 * plants_migrate_plan() (which mutates a table in place), then performs
 * the renames/NVS-name deletions the returned actions describe. Runs once
 * from plants_init() -- but NOT before other tasks can reach s_table: in
 * main.c, webserver_start() and wifi_manager_start() both run BEFORE
 * plants_init() in the hub-role boot sequence, so the HTTP server is
 * already live and api_v1.c's history_get -> plants_resolve_or_create()
 * bridge can legitimately race this function's table access (an earlier
 * revision of this comment claimed otherwise -- it was wrong). See
 * plants_run_migration() below for the s_mutex hold this requires around
 * plants_migrate_plan(), matching every other mutator's two-phase
 * discipline (file-top locking-invariant comment).
 *
 * Discovery has two independent legs, per the design spec's "for each
 * sensor with an NVS name OR an existing ring, create a plant":
 *   1. Legacy ring files directly under storage_base, named
 *      "<MAC12>_raw.bin" / "<MAC12>_hr.bin" -- the MAC-keyed naming
 *      storage.c used before M8 Task 3's P<id>-rekey (that task's own
 *      commit message: "key history rings by plant id"; NOT "_hourly.bin"
 *      -- tier_path() has only ever emitted "_hr.bin", the same suffix
 *      Task 3's report flagged plants_delete() for getting wrong once
 *      already).
 *   2. NVS sensor-name keys with no matching ring file at all (a sensor
 *      that was named but never sampled/never had history). app_config.c's
 *      name_key() keys these "nm_<MAC12>" in the shared "planthub"
 *      namespace -- an enumerable, recognisable prefix -- so these are NOT
 *      skipped: nvs_entry_find() walks every "nm_" key, and any mac not
 *      already found via leg 1 gets an input entry with has_raw/has_hourly
 *      false, has_name true (planner creates a plant, no renames).
 *
 * Idempotent by construction: after a successful migration the MAC-named
 * files are gone (renamed away) and the NVS name keys are erased, so a
 * second boot's discovery finds nothing and plants_migrate_plan() also
 * skips any mac defensively already present in the table -- either way,
 * `in` ends up empty or every mac in it is already known, so 0 actions. */

#define MIGRATE_MAX_INPUTS 32   /* generous headroom over PLANTS_MAX (16) */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex12_to_mac(const char *hex12, uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)((hex_val(hex12[i * 2]) << 4) | hex_val(hex12[i * 2 + 1]));
    }
}

/* True iff `s` is exactly 12 upper-case hex characters -- the
 * "^[0-9A-F]{12}" half of the legacy ring filename shape. */
static bool is_upper_hex12(const char *s)
{
    for (int i = 0; i < 12; i++) {
        if (hex_val(s[i]) < 0) return false;
    }
    return true;
}

typedef enum { LEGACY_NONE, LEGACY_RAW, LEGACY_HR } legacy_kind_t;

/* Matches "<MAC12>_raw.bin" / "<MAC12>_hr.bin" (storage.c's pre-Task-3
 * naming) and extracts the mac. Anything else -- including this boot's own
 * "P<id>_raw.bin"/"P<id>_hr.bin" files -- is LEGACY_NONE. */
static legacy_kind_t match_legacy_ring_name(const char *fname, uint8_t mac_out[6])
{
    size_t len = strlen(fname);
    if (len < 14 || fname[12] != '_' || !is_upper_hex12(fname)) return LEGACY_NONE;

    const char *suffix = fname + 13;
    legacy_kind_t kind;
    if (strcmp(suffix, "raw.bin") == 0) kind = LEGACY_RAW;
    else if (strcmp(suffix, "hr.bin") == 0) kind = LEGACY_HR;
    else return LEGACY_NONE;

    hex12_to_mac(fname, mac_out);
    return kind;
}

static migrate_input_t *migrate_find_or_add(migrate_input_t *ins, int *n, const uint8_t mac[6])
{
    for (int i = 0; i < *n; i++) {
        if (memcmp(ins[i].mac, mac, 6) == 0) return &ins[i];
    }
    if (*n >= MIGRATE_MAX_INPUTS) return NULL;
    migrate_input_t *e = &ins[*n];
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    (*n)++;
    return e;
}

/* Leg 1: scan storage_base for legacy ring files, one input entry per
 * distinct mac with has_raw/has_hourly set from whichever files exist. */
static void migrate_scan_ring_files(migrate_input_t *ins, int *n)
{
    if (!s_storage_base) return;   /* no filesystem: nothing to scan */

    DIR *d = opendir(s_storage_base);
    if (!d) {
        /* ENOENT for a fresh/empty storage dir is normal and not logged;
         * anything else (permission, mount trouble) is worth a line. */
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "migration: opendir(%s) failed: %s", s_storage_base, strerror(errno));
        }
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint8_t mac[6];
        legacy_kind_t kind = match_legacy_ring_name(de->d_name, mac);
        if (kind == LEGACY_NONE) continue;

        migrate_input_t *in = migrate_find_or_add(ins, n, mac);
        if (!in) {
            ESP_LOGW(TAG, "migration: too many distinct legacy macs discovered; dropping %s", de->d_name);
            continue;
        }
        if (kind == LEGACY_RAW) in->has_raw = true;
        else in->has_hourly = true;
    }
    closedir(d);
}

/* Leg 2: fill in `has_name`/`name` for every mac leg 1 already found, then
 * enumerate NVS "nm_<MAC12>" keys (app_config.c's name_key() scheme, in the
 * shared "planthub" namespace) for named-but-fileless macs leg 1 never
 * saw. */
static void migrate_scan_names(migrate_input_t *ins, int *n)
{
    char name[33];
    for (int i = 0; i < *n; i++) {
        if (app_config_get_sensor_name(ins[i].mac, name)) {
            ins[i].has_name = true;
            strlcpy(ins[i].name, name, sizeof(ins[i].name));
        }
    }

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS, NVS_TYPE_STR, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        /* "nm_" + 12 hex chars, exactly -- app_config.c's name_key(). */
        if (strncmp(info.key, "nm_", 3) == 0 && strlen(info.key) == 15 &&
            is_upper_hex12(info.key + 3)) {
            uint8_t mac[6];
            hex12_to_mac(info.key + 3, mac);

            if (migrate_find_or_add(ins, n, mac) == NULL && *n >= MIGRATE_MAX_INPUTS) {
                ESP_LOGW(TAG, "migration: too many distinct legacy macs discovered "
                              "(NVS-name-only); dropping nm_%s", info.key + 3);
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    /* migrate_find_or_add() above may have just created fresh entries for
     * NVS-name-only macs (has_name still false, from the memset in
     * migrate_find_or_add()) -- fill those in exactly like leg 1's macs. */
    for (int i = 0; i < *n; i++) {
        if (!ins[i].has_name && app_config_get_sensor_name(ins[i].mac, name)) {
            ins[i].has_name = true;
            strlcpy(ins[i].name, name, sizeof(ins[i].name));
        }
    }
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Renames one legacy ring file to its P<id> name, unless the destination
 * is already occupied -- which the persist-before-execute ordering in
 * plants_run_migration() should make impossible in the ordinary case, but
 * a prior crash mid-migration (or manual tampering) could still leave a
 * P<id> file behind for an id this run is about to reuse... except ids are
 * never reused (plants_table.h), so a collision here always means
 * something unexpected is on disk. Either way, silently clobbering it
 * would be a silent cross-plant history overwrite -- log and keep the
 * legacy file untouched instead; it stays discoverable (still MAC-named)
 * for a future migration attempt or manual recovery. */
static void migrate_rename_one(const char *oldp, const char *newp)
{
    if (path_exists(newp)) {
        ESP_LOGW(TAG, "migration: %s already exists; leaving %s in place rather than "
                      "overwriting it", newp, oldp);
        return;
    }
    if (rename(oldp, newp) != 0) {
        ESP_LOGW(TAG, "migration: rename %s -> %s failed: %s", oldp, newp, strerror(errno));
    }
}

/* Executes one planned action: rename() the ring files that exist, delete
 * the NVS name key on a successful move_name. Per spec: log a failure and
 * continue -- never let one bad file/key abort the rest of the boot's
 * migration. Table access happens above in plants_run_migration(), not
 * here -- this function only touches the filesystem and NVS.
 *
 * oldp/newp are `static`, not stack locals (post-mortem stack-safety fix,
 * same rationale as this file's other init-path buffers): this function is
 * only ever called from plants_run_migration()'s single-threaded,
 * once-per-boot loop, one action at a time, so a single shared pair is
 * safe -- the rename_raw and rename_hourly blocks below never run
 * concurrently with each other or with a later call. */
static void migrate_execute_action(const migrate_action_t *a)
{
    char mac_hex[13];
    snprintf(mac_hex, sizeof(mac_hex), "%02X%02X%02X%02X%02X%02X",
             a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);

    static char oldp[128], newp[128];
    if (a->rename_raw && s_storage_base) {
        snprintf(oldp, sizeof(oldp), "%s/%s_raw.bin", s_storage_base, mac_hex);
        snprintf(newp, sizeof(newp), "%s/P%u_raw.bin", s_storage_base, a->plant_id);
        migrate_rename_one(oldp, newp);
    }
    if (a->rename_hourly && s_storage_base) {
        snprintf(oldp, sizeof(oldp), "%s/%s_hr.bin", s_storage_base, mac_hex);
        snprintf(newp, sizeof(newp), "%s/P%u_hr.bin", s_storage_base, a->plant_id);
        migrate_rename_one(oldp, newp);
    }
    if (a->move_name) {
        esp_err_t err = app_config_clear_sensor_name(a->mac);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "migration: failed to clear NVS name for %s: %s", mac_hex, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "migration: mac %s -> plant %u (raw=%d hourly=%d name=%d)",
             mac_hex, a->plant_id, a->rename_raw, a->rename_hourly, a->move_name);
}

/* Runs from plants_init(), between the blob load and the summary log
 * below. s_table already holds whatever load_file()/plants_adopt_legacy_nvs()
 * found (normally empty; plants_migrate_plan() skips any mac already in it
 * either way).
 *
 * Ordering is deliberately persist-BEFORE-execute, not the more obvious
 * "do the renames, then save what happened" -- plants_migrate_plan()
 * already mutates the table (new plants, names, next_id) the moment it
 * returns, so persist_table() commits that mutation to flash immediately,
 * and only THEN do the actual rename()/NVS-delete side effects run.
 *
 * The failure story this avoids: rename-then-persist leaves a window
 * where a crash after a successful rename (+ NVS name-key erase) but
 * before the trailing persist_table() reverts the table on reboot -- the
 * P<id> files that already got renamed no longer match the legacy
 * MAC-named-file scan (they're already renamed away), and the NVS name is
 * already erased, so that plant's entire history is permanently orphaned
 * with nothing left pointing to it. Worse: next_id also reverts to
 * whatever it was before, so the next boot's migration re-allocates that
 * SAME id to whichever mac it processes next -- not necessarily the same
 * one, since directory iteration order is not guaranteed stable across
 * boots -- and THAT mac's rename() silently overwrites the orphaned
 * P<id> file with a completely different mac's history: a silent
 * cross-plant history overwrite, violating plants_table.h's
 * never-reused-id invariant.
 *
 * Persist-first bounds the damage from a mid-migration crash to, at
 * worst, one file left stuck under its legacy MAC name: the table (and
 * therefore next_id) is already durable on flash before any rename runs,
 * so a re-run's plants_migrate_plan() finds that mac already known via
 * find_mac() and skips it outright -- dead weight, never touched again,
 * never clobbered.
 *
 * If persist_table() itself fails, none of the actions run this boot:
 * executing them anyway would let files move (and NVS names disappear)
 * out from under a table that never reached flash, which is exactly the
 * unrecoverable case above waiting to happen on the next crash. s_table's
 * RAM copy still reflects the plan -- same "RAM can run ahead of flash on
 * a failed commit" contract as every other mutator in this file -- and
 * the migration simply retries from scratch next boot. */
static void plants_run_migration(void)
{
    static migrate_input_t inputs[MIGRATE_MAX_INPUTS];
    memset(inputs, 0, sizeof(inputs));
    int n_inputs = 0;

    migrate_scan_ring_files(inputs, &n_inputs);
    migrate_scan_names(inputs, &n_inputs);
    if (n_inputs == 0) return;

    /* Phase 1 (RAM only, s_mutex held) -- same two-phase split as
     * plants_rename()/plants_assign()/plants_create()/plants_delete()
     * above: plants_migrate_plan() only ever touches *t (the `in`/`out`
     * arrays are this function's own local buffers, never shared), so the
     * lock only needs to span this one call, not the file/NVS work below.
     * `static`, not a stack local, matching `inputs` above -- same
     * post-mortem stack-safety fix as this file's other init-path buffers:
     * plants_run_migration() only ever runs once, synchronously, from
     * plants_init(). */
    static migrate_action_t actions[PLANTS_MAX];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n_actions = plants_migrate_plan(&s_table, inputs, n_inputs, actions, PLANTS_MAX);
    xSemaphoreGive(s_mutex);
    if (n_actions == 0) return;

    /* Phase 2: persist BEFORE executing any action -- see this function's
     * own comment above for why. */
    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "migration: failed to persist the plant table (%s) before executing any "
                      "action -- skipping execution this boot rather than renaming/erasing "
                      "state out from under a table that never reached flash; RAM already "
                      "reflects the plan and this will retry from scratch next boot",
                 esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < n_actions; i++) {
        migrate_execute_action(&actions[i]);
    }
    ESP_LOGI(TAG, "migration: %d plant(s) migrated from sensor-keyed state", n_actions);
}

/* plants_last_values() cache helpers -- see s_last_values' declaration
 * comment for the slot-capacity argument. Moved ahead of
 * plants_init()/plants_resolve_or_create()/plants_assign() (final M8 review,
 * M1) so plants_assign() below can call lv_cache_drop() without a forward
 * declaration -- it used to only be needed from plants_delete(), further
 * down. */
static int lv_cache_find(uint8_t id)
{
    for (int i = 0; i < LAST_VALUES_CACHE_CAP; i++) {
        if (s_last_values[i].used && s_last_values[i].id == id) return i;
    }
    return -1;
}

static void lv_cache_drop(uint8_t id)
{
    xSemaphoreTake(s_lv_mutex, portMAX_DELAY);
    int idx = lv_cache_find(id);
    if (idx >= 0) s_last_values[idx].used = false;
    xSemaphoreGive(s_lv_mutex);
}

/* One-boot self-migration (this fix, not M8 Task 4's sensor-keyed migration
 * below): adopts an existing M8-era NVS "plants" blob into plants.bin, for a
 * live rig that already has a plant table sitting in NVS from before this
 * fix landed. Only ever does anything when plants.bin does not exist yet
 * (load_file() already returned false) -- once plants.bin exists, this is
 * skipped every subsequent boot without even opening NVS. Write-then-erase
 * ordering, deliberately: the file must be durably on disk before the NVS
 * key that's the only other copy of this data gets erased, or a crash
 * between the two would drop the table entirely with no copy left anywhere.
 * If the file write fails, the NVS key is left in place so this retries
 * from scratch next boot -- see plants_run_migration()'s own persist-before-
 * execute comment above for the same reasoning applied to a different pair
 * of side effects.
 *
 * M2 Task 4 review: this used to load into a separate `static
 * plants_table_t legacy`, explicitly plants_table_init()'d first, then copy
 * it into s_table on success (`s_table = legacy`). That copy -- and the
 * ~1953-byte buffer behind it -- is unnecessary: plants_init() only calls
 * this function when load_file() already returned false, and load_file()
 * never partially writes *out on failure (see its own doc comment), so
 * s_table is guaranteed to still hold the plants_table_init() defaults
 * plants_init() set before either load attempt. load_legacy_nvs_blob()
 * itself has the identical contract (only writes *out on success -- see its
 * doc comment), so unpacking straight into s_table is safe: on success
 * s_table gets the legacy contents (same as before); on failure s_table is
 * simply left at the defaults it already holds, a no-op re-application, not
 * a new write. This exact function was the innermost frame of the call
 * chain a hardware crash-loop hit a "Stack protection fault" in, on the
 * main task -- see plants_init()'s top comment -- so removing a buffer here
 * keeps, rather than reopens, that fix: s_table was always `static` (it's
 * the file-scope live table), so this isn't reintroducing a stack local. */
static void plants_adopt_legacy_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;  /* fresh "planthub" namespace: nothing to adopt */
    }
    bool have_legacy = load_legacy_nvs_blob(h, &s_table);
    nvs_close(h);
    if (!have_legacy) return;

    esp_err_t werr = persist_table();
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "plants: adopted a legacy NVS plant table but failed to write %s/%s (%s); "
                      "keeping the legacy NVS key so this retries next boot",
                 s_storage_base, PLANTS_FILE_NAME, esp_err_to_name(werr));
        return;
    }

    nvs_handle_t wh;
    esp_err_t eerr = nvs_open(NS, NVS_READWRITE, &wh);
    if (eerr == ESP_OK) {
        eerr = nvs_erase_key(wh, KEY_PLANTS);
        if (eerr == ESP_OK) eerr = nvs_commit(wh);
        nvs_close(wh);
    }
    if (eerr != ESP_OK) {
        ESP_LOGW(TAG, "plants: adopted a legacy NVS plant table into %s/%s but failed to erase "
                      "the now-redundant NVS key (%s); it will be ignored (plants.bin already "
                      "exists) but not cleaned up until this succeeds",
                 s_storage_base, PLANTS_FILE_NAME, esp_err_to_name(eerr));
        return;
    }
    ESP_LOGI(TAG, "plants: migrated the plant table from NVS to %s/%s (19-stale-copies fix); "
                  "legacy NVS key erased", s_storage_base, PLANTS_FILE_NAME);
}

/* ---------------- Stack-safety post-mortem (hardware bring-up, follow-up to the LittleFS fix) ----------------
 *
 * The first hardware run of the LittleFS fix above crash-looped: a "Stack
 * protection fault" on the "main" task, MEPC decoding into
 * snprintf/_svfprintf_r, firing immediately after load_file()'s
 * "plants.bin not readable" WARN -- i.e. inside this function's post-load
 * path (the plants_adopt_legacy_nvs() one-boot NVS-adoption call below).
 * And because the crash landed BEFORE that path's file write, the legacy
 * NVS key survived every reboot, so the crash repeated forever: a
 * permanent boot loop with no self-recovery.
 *
 * Root cause: this whole load/adopt sequence stacked too much on the main
 * task. load_file()'s ~674-byte plants_blob_t local, plants_adopt_legacy_nvs()'s
 * ~673-byte plants_table_t local, load_legacy_nvs_blob()'s own ~674-byte
 * plants_blob_t local, and (before this fix) unpack_blob()'s OWN ~673-byte
 * intermediate copy were all simultaneously reachable in nested calls on
 * top of whatever main.c's boot sequence already held on that same task's
 * stack -- on top of which ESP_LOGW()'s own internal vsnprintf-family
 * formatting (the snprintf/_svfprintf_r the fault decoded into) needed
 * just enough more to tip it over. The pre-fix NVS path never had this
 * problem: NVS's blob API reads straight into one caller-supplied buffer,
 * no comparable nesting.
 *
 * Fix: every plants_table_t / plants_blob_t / migrate_action_t buffer
 * reachable from this function -- load_file()'s and load_legacy_nvs_blob()'s
 * blobs, plants_adopt_legacy_nvs()'s legacy table, plants_run_migration()'s
 * actions array, migrate_execute_action()'s path buffers, and write_file()'s
 * path buffers -- moved to `static` storage (each function's own doc
 * comment says why that specific one is safe: either this function's
 * single-invocation/single-task guarantee, or, for persist_table()/
 * write_file(), s_persist_mutex now serialising the whole snapshot+write).
 * unpack_blob()'s intermediate copy was removed outright rather than made
 * static -- it never needed to exist. See persist_table()'s own comment: the
 * exact same "big locals in a function reachable from a small task stack"
 * shape also very likely explains the ORIGINAL M8 hardware incident's crash
 * (the sampler task's 4096-byte stack, under normal sampling load), not
 * just the NVS-GC blast radius that motivated this file's move off NVS in
 * the first place -- two independent contributing causes to the same
 * symptom, not one.
 *
 * Follow-up (M2 Task 4 review): per-plant size roughly tripled (capability
 * bindings, plants_table.h), which made three of these `static` buffers
 * -- persist_table()'s now-removed `snapshot`, and the now-shared
 * load_file()/load_legacy_nvs_blob() blobs -- worth consolidating (see
 * their own doc comments and task-4-report.md's "Fix round" section for the
 * measured numbers). None of this reopens the bug this section describes:
 * every buffer that was `static` before still is, just fewer of them --
 * nothing moved back onto a task stack. */
/* M2 Task 4 hardware hotfix, round 4. main.c's boot heap trace measured
 * plants_init() costing 7956 B of heap that is never returned -- an amount
 * that nothing in this function's source obviously accounts for (three
 * mutexes are ~264 B; every plants_table_t/plants_blob_t buffer here is
 * file-scope .bss, not heap; and esp_littlefs frees each
 * vfs_littlefs_file_t on fclose()). Rather than guess, this splits the one
 * measurement into three, so a single boot log says whether the cost is in
 * the mutexes, the plants.bin load (stdio/VFS/LittleFS first-open
 * allocations, which newlib and esp_littlefs both retain in per-process
 * pools), or plants_run_migration()'s directory scan. Permanent, same
 * rationale as main.c's log_heap(). */
static void plants_log_heap(const char *milestone)
{
    ESP_LOGI(TAG, "heap @ plants_init %s: free=%u B largest_free_block(8BIT|INTERNAL)=%u B",
             milestone, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

esp_err_t plants_init(const char *storage_base)
{
    plants_log_heap("entry");
    s_mutex = xSemaphoreCreateMutex();
    s_persist_mutex = xSemaphoreCreateMutex();
    s_lv_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || !s_persist_mutex || !s_lv_mutex) return ESP_ERR_NO_MEM;

    s_storage_base = storage_base;
    memset(s_full_logged, 0, sizeof(s_full_logged));
    memset(s_last_values, 0, sizeof(s_last_values));
    plants_table_init(&s_table);

    if (s_storage_base) {
        if (!load_file(&s_table)) {
            /* plants.bin doesn't exist (or is unreadable/corrupt): try the
             * one-boot legacy-NVS adoption before accepting the empty
             * defaults load_file() already left in s_table. A live M8-era
             * rig's table gets recovered here; a genuinely fresh install
             * finds no legacy key either and stays empty. */
            plants_adopt_legacy_nvs();
        }
    } else {
        /* No filesystem -- see plants.h: plants are hub-only and the hub
         * always has storage in practice, so this only happens if littlefs
         * itself failed to mount. RAM-only table for the rest of this boot;
         * persist_table() below (and on every mutation) is a silent no-op
         * from here on, this is the one and only WARN for that. */
        ESP_LOGW(TAG, "plants: no storage_base (littlefs unavailable) -- plant table is "
                      "RAM-only and will not survive a reboot");
    }

    plants_log_heap("after table load");

    plants_run_migration();
    plants_log_heap("after migration scan");

    int count = 0;
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (s_table.p[i].in_use) count++;
    }
    ESP_LOGI(TAG, "plants_init: %d plant(s) loaded, next_id=%u, storage_base=%s",
             count, s_table.next_id, s_storage_base ? s_storage_base : "(none)");
    return ESP_OK;
}

uint8_t plants_resolve_or_create(const uint8_t mac[6])
{
    /* s_mutex is NULL until plants_init() has run, which only happens on
     * the hub role (main.c, M8 Task 2). webserver_start() -- and therefore
     * this function's M8 Task 3 caller in api_v1.c's history_get -- runs on
     * every role, including the portal fallback a NODE with a failed pairing
     * attempt lands in (main.c's role != SWARM_ROLE_NODE guard around
     * plants_init() skips it there). Without this check, a plant-less
     * device hitting that route would xSemaphoreTake() a NULL handle
     * instead of getting the same "no plant" answer plants_resolve_or_create
     * gives for any other lookup failure. */
    if (!mac || !s_mutex) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = plants_table_find_mac(&s_table, mac);
    if (idx >= 0) {
        uint8_t id = s_table.p[idx].id;
        xSemaphoreGive(s_mutex);
        return id;
    }
    uint8_t new_id = plants_table_create(&s_table, mac);
    xSemaphoreGive(s_mutex);

    if (new_id == 0) {
        log_full_once(mac);
        return 0;
    }

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_resolve_or_create: plants.bin write failed (%s); RAM cache already "
                      "reflects the new plant and will not revert until the next successful "
                      "write or a reboot", esp_err_to_name(err));
    }
    return new_id;
}

/* See plants.h for the full derivation (H1+M3, final M8 review). Deliberately
 * takes NO plants_table_t snapshot of its own (re-review fix: a 673-byte
 * plants_table_t on the CALLER's stack is unsafe here -- unlike sample_once(),
 * which keeps its own snapshots `static` for exactly this reason, this
 * function is called from two different task contexts with two different
 * stacks: the sampler task (4096 bytes) AND the httpd task, which has a
 * documented exhaustion history. A shared `static` table would also race
 * between those two tasks, since nothing serialises them against each
 * other). plants_resolve_or_create() already does its own find-or-create
 * atomically under s_mutex, so pre-checking plants_table_find_mac() against
 * a locally-snapshotted table was redundant -- a benign double-check at
 * best, a stale answer (racing a concurrent assign/delete) at worst.
 * Bounded REGISTRY_MAX_SENSORS mutex-guarded lookups; no allocation, no
 * caller-stack table. */
void plants_adopt_from_registry(const legacy_registry_t *reg, uint32_t now_uptime_s, uint32_t liveness_s)   /* M2-SHIM */
{
    if (!reg) return;
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        const sensor_entry_t *e = &reg->sensors[i];
        if (!e->in_use) continue;
        /* Liveness gate (H1): skip a probe that's gone dark rather than
         * adopting it into an undeletable ghost plant -- see this
         * function's doc comment in plants.h. */
        if (now_uptime_s - e->last_seen_s > liveness_s) continue;
        plants_resolve_or_create(e->mac);
    }
}

void plants_snapshot(plants_table_t *out)
{
    if (!out) return;
    /* Same "uninitialised registry" guard as plants_resolve_or_create()
     * above (M8 Task 6, forward note from Task 2's review): s_mutex is NULL
     * until plants_init() has run, which only happens on the hub role --
     * but webserver_start() (and therefore every GET /api/v1/plants
     * request) runs on every role, including a NODE that fell back to its
     * setup portal after a failed pairing attempt. Without this,
     * that portal's plants routes would xSemaphoreTake() a NULL handle
     * (FreeRTOS asserts/crashes on that) instead of getting the same safe
     * "no plants" answer every other reader here gives -- an empty,
     * freshly plants_table_init()'d table is exactly that answer. */
    if (!s_mutex) {
        plants_table_init(out);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_table;
    xSemaphoreGive(s_mutex);
}

size_t plants_ids(uint8_t *out, size_t max)
{
    if (!out) return 0;
    /* Same "uninitialised registry" guard as plants_snapshot() above. */
    if (!s_mutex) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = 0;
    for (int i = 0; i < PLANTS_MAX && n < max; i++) {
        if (s_table.p[i].in_use) out[n++] = s_table.p[i].id;
    }
    xSemaphoreGive(s_mutex);
    return n;
}

esp_err_t plants_rename(uint8_t id, const char *name)
{
    if (!name || strlen(name) > PLANT_NAME_LEN) return ESP_ERR_INVALID_ARG;
    /* Same uninitialised-registry guard as plants_snapshot() above: with no
     * plant table loaded, every id is "unknown" -- the same answer
     * plants_table_rename() gives for an id that just isn't present, so
     * this reuses that exact return value rather than inventing a new one. */
    if (!s_mutex) return ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = plants_table_rename(&s_table, id, name);
    xSemaphoreGive(s_mutex);
    if (!ok) return ESP_ERR_NOT_FOUND;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_rename(%u): plants.bin write failed (%s); RAM cache already reflects "
                      "the new name and will not revert until the next successful write or a "
                      "reboot", id, esp_err_to_name(err));
    }
    return err;
}

esp_err_t plants_assign(uint8_t id, const uint8_t *mac_or_null)   /* M2-SHIM: see plants.h */
{
    /* Same uninitialised-registry guard as plants_rename() above. */
    if (!s_mutex) return ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Capture which plant (if any) currently holds mac_or_null BEFORE the
     * mutation below -- plants_table_assign()'s "assigning a mac already
     * assigned elsewhere MOVES it" semantics (plants_table.h) means that
     * plant loses its probe as a side effect of this one call, so its
     * plants_last_values() cache entry needs dropping too, not just id's
     * (final M8 review, M1). NULL mac_or_null (unassign) has nothing to
     * look up here -- other_id stays 0. */
    int other_idx = mac_or_null ? plants_table_find_mac(&s_table, mac_or_null) : -1;
    uint8_t other_id = (other_idx >= 0) ? s_table.p[other_idx].id : 0;
    bool ok = plants_table_assign(&s_table, id, mac_or_null);
    xSemaphoreGive(s_mutex);
    if (!ok) return ESP_ERR_NOT_FOUND;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_assign(%u): plants.bin write failed (%s); RAM cache already reflects "
                      "the new assignment and will not revert until the next successful write "
                      "or a reboot", id, esp_err_to_name(err));
    }
    /* Drop BOTH the target plant's cache slot and the losing plant's (M1):
     * without this, a last_values hit cached before this call could keep
     * serving a plant's old probe reading after that plant is later
     * unassigned -- exactly the stale pre-assignment tail plants_delete()'s
     * own lv_cache_drop() already guards against for a deleted id. Done
     * regardless of the persist result above, same as plants_delete(): the
     * RAM table (and therefore what SHOULD be cached) already moved. */
    lv_cache_drop(id);
    if (other_id != 0 && other_id != id) lv_cache_drop(other_id);
    return err;
}

uint8_t plants_create(void)
{
    /* Same uninitialised-registry guard as plants_rename() above -- reuses
     * 0, this function's own existing "can't create" contract (normally
     * "table full"; here "no table at all" reads the same way to a caller,
     * and the API layer's 409 "plant table full" response is an honest
     * enough answer for a route that genuinely cannot create a plant right
     * now either way). */
    if (!s_mutex) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t id = plants_table_create(&s_table, NULL);
    xSemaphoreGive(s_mutex);
    if (id == 0) return 0;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_create: plants.bin write failed (%s); RAM cache already reflects the "
                      "new plant and will not revert until the next successful write or a "
                      "reboot", esp_err_to_name(err));
    }
    return id;
}

/* Removes P<id>_raw.bin/P<id>_hr.bin under the base path recorded at
 * plants_init() -- the plant-keyed layout Task 3 rekeys storage.c to.
 * Neither file may exist yet (a plant with no history, or before Task 3
 * lands), so ENOENT is silently ignored; any other error is only logged, as
 * this is best-effort cleanup after the table mutation (persisted above)
 * has already succeeded and must not be undone by a filesystem hiccup. */
static void remove_ring_files(uint8_t id)
{
    if (!s_storage_base) return;  /* storage unavailable: nothing to remove */

    char path[128];
    snprintf(path, sizeof(path), "%s/P%u_raw.bin", s_storage_base, id);
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "plants_delete(%u): failed to remove %s: %s", id, path, strerror(errno));
    }
    snprintf(path, sizeof(path), "%s/P%u_hr.bin", s_storage_base, id);
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "plants_delete(%u): failed to remove %s: %s", id, path, strerror(errno));
    }
}

esp_err_t plants_delete(uint8_t id)
{
    /* Same uninitialised-registry guard as plants_rename()/plants_assign()
     * above. remove_ring_files()/lv_cache_drop() below are both no-ops in
     * this state too (s_storage_base is NULL until plants_init() runs, and
     * s_lv_mutex is NULL so lv_cache_drop() would itself need the same
     * guard -- moot here since we return before reaching it). */
    if (!s_mutex) return ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = plants_table_delete(&s_table, id);
    xSemaphoreGive(s_mutex);
    if (!ok) return ESP_ERR_NOT_FOUND;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_delete(%u): plants.bin write failed (%s); RAM cache already reflects "
                      "the deletion and will not revert until the next successful write or a "
                      "reboot", id, esp_err_to_name(err));
    }

    remove_ring_files(id);
    /* Frees id's storage.c write-cursor cache slot(s) for reuse by a future
     * plant (M8 Task 6 review fix -- MEDIUM 3a). Ids are never reused
     * (plants_table.h), so without this a deleted plant's slot(s) sit
     * claimed forever -- storage.c's per-tier cache is small and fixed-size
     * (CACHE_SLOTS), so enough create/delete cycles would eventually leave
     * no free slot for ANY plant's appends, live or not. Not gated on
     * s_storage_base (unlike remove_ring_files() above): this only touches
     * storage.c's in-RAM cache, not the filesystem, and storage_drop() is a
     * safe no-op for an id with no cached slot either way. */
    storage_drop(id);
    /* plants_last_values()'s RAM cache is dropped here too: ids are never
     * reused (plants_table.h), so this id will never be looked up again as
     * a live plant, but a stray caller holding onto a stale id must still
     * see "no history" rather than whatever was cached before the delete. */
    lv_cache_drop(id);
    return err;
}

/* ---------------- Capability bindings (M2 Task 4) ----------------
 *
 * Same two-phase discipline as plants_rename()/plants_assign()/etc. above:
 * mutate s_table under s_mutex (a bounded, allocation-free
 * plants_table_bind_cap() call), release s_mutex, THEN persist_table() --
 * which re-reads the current s_table under its own brief s_mutex hold, so
 * this never holds s_mutex across the file write. */

bool plants_bind_cap(uint8_t plant_id, uint8_t cap_id, const device_id_t *dev)
{
    /* Same uninitialised-registry guard as plants_rename() above. */
    if (!s_mutex) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = plants_table_bind_cap(&s_table, plant_id, cap_id, dev);
    xSemaphoreGive(s_mutex);
    if (!ok) return false;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_bind_cap(%u, cap %u): plants.bin write failed (%s); RAM cache already "
                      "reflects the new binding and will not revert until the next successful "
                      "write or a reboot", plant_id, cap_id, esp_err_to_name(err));
    }
    return true;
}

int plants_bind_device(uint8_t plant_id, const device_id_t *dev, const registry_t *reg)
{
    if (!dev || !reg || !s_mutex) return 0;

    int ridx = registry_find(reg, dev);
    if (ridx < 0) return 0;   /* dev not in this snapshot: nothing to bind */
    const device_entry_t *de = &reg->devices[ridx];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = 0;
    for (uint8_t cap = 0; cap < CAPABILITY_COUNT; cap++) {
        if (de->caps[cap].valid && plants_table_bind_cap(&s_table, plant_id, cap, dev)) {
            count++;
        }
    }
    xSemaphoreGive(s_mutex);
    if (count == 0) return 0;   /* unknown plant_id, or dev reports nothing yet */

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_bind_device(%u): plants.bin write failed (%s); RAM cache already "
                      "reflects %d new binding(s) and will not revert until the next successful "
                      "write or a reboot", plant_id, esp_err_to_name(err), count);
    }
    return count;
}

size_t plants_bindings(uint8_t plant_id, plant_binding_t *out, size_t max)
{
    if (!out || !s_mutex) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = plants_table_bindings(&s_table, plant_id, out, max);
    xSemaphoreGive(s_mutex);
    return n;
}

bool plants_cap_value(uint8_t plant_id, uint8_t cap_id, const registry_t *reg,
                      float *value_out, uint32_t *age_s_out)
{
    if (!reg || cap_id >= CAPABILITY_COUNT || !s_mutex) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = plants_table_find_id(&s_table, plant_id);
    bool bound = idx >= 0 && s_table.p[idx].cap_bound[cap_id];
    device_id_t dev = bound ? s_table.p[idx].cap_dev[cap_id] : (device_id_t){ 0 };
    xSemaphoreGive(s_mutex);
    if (!bound) return false;

    int ridx = registry_find(reg, &dev);
    if (ridx < 0) return false;   /* bound device isn't in this snapshot */

    const cap_slot_t *slot = &reg->devices[ridx].caps[cap_id];
    if (!slot->valid) return false;   /* no value in this slot yet */

    if (value_out) *value_out = capability_decode(cap_id, slot->raw);
    if (age_s_out) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        *age_s_out = (now >= slot->updated_s) ? now - slot->updated_s : 0;
    }
    return true;
}

/* Same boot-table resolve api_v1.c's history_get wires storage_query's
 * resolve_fn to (its resolve_shim there does the identical one-line
 * forward) -- boot_id/rel_s -> wall-clock epoch is timekeeper's job, not
 * storage.c's or plants.c's. */
static bool lv_resolve(void *rctx, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out)
{
    (void)rctx;
    return timekeeper_resolve(boot_id, rel_s, epoch_out);
}

typedef struct {
    bool             found;
    uint32_t         epoch;
    storage_rec_v1_t rec;   /* M2-SHIM */
} lv_scan_ctx_t;

/* storage_query() emits rows in ring order, strictly oldest-to-newest (see
 * storage.c and test_storage.c's wraparound-order assertions) -- so simply
 * overwriting on every call, with no comparison, leaves the newest row in
 * *ctx once the scan completes. */
static void lv_row(void *vctx, uint32_t epoch, const storage_rec_v1_t *rec)   /* M2-SHIM */
{
    lv_scan_ctx_t *c = vctx;
    c->found = true;
    c->epoch = epoch;
    c->rec = *rec;
}

bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,   /* M2-SHIM: see plants.h */
                        uint32_t *lux, uint16_t *conductivity, uint8_t *battery,
                        uint32_t *epoch_out)
{
    /* Same "uninitialised registry" guard as plants_resolve_or_create()
     * above -- s_lv_mutex is NULL until plants_init() (hub role only) has
     * run. No caller reaches this yet (Task 3 does not wire one up), but
     * the guard costs nothing and keeps this function safe to call from any
     * role the moment Tasks 5/6 do wire it up. */
    if (id == 0 || !s_lv_mutex) return false;

    xSemaphoreTake(s_lv_mutex, portMAX_DELAY);
    int idx = lv_cache_find(id);
    if (idx >= 0) {
        /* Every cached slot is a genuine hit (see below -- a miss is never
         * written), so there's nothing left to check beyond "is it there". */
        last_values_cache_t c = s_last_values[idx];
        xSemaphoreGive(s_lv_mutex);
        if (temp_dc)       *temp_dc = c.temp_dc;
        if (moisture)      *moisture = c.moisture_pct;
        if (lux)           *lux = c.lux;
        if (conductivity)  *conductivity = c.conductivity_us;
        if (battery)       *battery = c.battery_pct;
        if (epoch_out)     *epoch_out = c.epoch;
        return true;
    }
    xSemaphoreGive(s_lv_mutex);

    /* Cache miss: scan storage.c's raw ring for this plant id. Widest
     * possible epoch range -- this call wants the ring tail, not a
     * window -- and storage_query() itself tolerates a NULL/unmounted
     * s_storage_base the same way it tolerates a plant with no ring file
     * yet: fopen() fails, 0 rows emitted, "not found" below. */
    lv_scan_ctx_t scan = { .found = false };
    if (s_storage_base) {
        storage_query_v1(s_storage_base, id, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu,   /* M2-SHIM */
                         lv_resolve, NULL, lv_row, &scan);
    }

    /* Cache ONLY a genuine hit obtained under a synced clock (M8 Task 6
     * review fix -- MEDIUM 2). This used to cache found=false too ("found
     * or not", keyed by id) on the theory that a plant with genuinely no
     * history shouldn't re-scan its non-existent ring file forever. That
     * reasoning breaks the moment the clock isn't synced yet: every record
     * in the ring becomes momentarily unresolvable (lv_resolve() ->
     * timekeeper_resolve() fails for every row, storage_query() skips them
     * all -- see storage.c), so scan.found comes back false for a plant
     * that has REAL history, and the old first-read-wins cache pinned that
     * false result for the rest of the boot -- a GET that merely raced NTP
     * would permanently blank a probe-less plant's last values, exactly
     * the "nothing is blanked" guarantee plants.h promises. Not caching a
     * miss (synced or not) costs at most a repeat scan of one plant's raw
     * ring on the next call -- bounded and cheap -- versus a wrong answer
     * that sticks until reboot. */
    if (scan.found && timekeeper_synced()) {
        xSemaphoreTake(s_lv_mutex, portMAX_DELAY);
        idx = lv_cache_find(id);
        if (idx < 0) {
            for (int i = 0; i < LAST_VALUES_CACHE_CAP; i++) {
                if (!s_last_values[i].used) { idx = i; break; }
            }
        }
        if (idx >= 0) {
            last_values_cache_t *c = &s_last_values[idx];
            c->used = true;
            c->id = id;
            c->found = true;
            c->temp_dc = scan.rec.temp_dc;
            c->moisture_pct = scan.rec.moisture_pct;
            c->lux = scan.rec.lux;
            c->conductivity_us = scan.rec.conductivity_us;
            c->battery_pct = scan.rec.battery_pct;
            c->epoch = scan.epoch;
        } else {
            /* Cache exhausted -- cannot happen in practice (capacity ==
             * PLANTS_MAX and plants_delete() always frees its slot), but if
             * it ever does this call still answers correctly, it just
             * re-scans storage on every future call for this id instead of
             * caching. */
            ESP_LOGW(TAG, "plants_last_values(%u): last-values cache full; not caching this result", id);
        }
        xSemaphoreGive(s_lv_mutex);
    }

    if (!scan.found) return false;
    if (temp_dc)       *temp_dc = scan.rec.temp_dc;
    if (moisture)      *moisture = scan.rec.moisture_pct;
    if (lux)           *lux = scan.rec.lux;
    if (conductivity)  *conductivity = scan.rec.conductivity_us;
    if (battery)       *battery = scan.rec.battery_pct;
    if (epoch_out)      *epoch_out = scan.epoch;
    return true;
}
