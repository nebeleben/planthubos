#include "plants.h"
#include "plants_migrate.h"
#include "storage.h"
#include "timekeeper.h"
#include "app_config.h"
#include "esp_log.h"
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

/* On-disk format of the KEY_PLANTS blob. Bump this and add a migration
 * branch in load_blob() below whenever plant_entry_t's shape changes --
 * same reasoning as swarm_store.c's SWARM_STORE_FORMAT for the node table.
 * Deliberately NOT plants_table_t dumped raw: that struct's `bool` fields
 * and any compiler-inserted padding are not a stable on-disk shape, so this
 * mirror struct pins every field to an explicit width/order instead. */
#define PLANTS_BLOB_FORMAT 1

typedef struct __attribute__((packed)) {
    uint8_t id;
    uint8_t in_use;      /* bool, packed as u8 */
    uint8_t mac[6];
    uint8_t mac_valid;   /* bool, packed as u8 */
    char    name[PLANT_NAME_LEN + 1];
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
 *     bounded, allocation-free struct copy/mutation, never flash I/O, and
 *     never held while also contending for s_nvs_mutex. plants_snapshot()
 *     and every other reader can therefore never be made to wait on a
 *     flash commit, no matter what else is happening concurrently.
 *
 *   - s_nvs_mutex guards the actual NVS write + nvs_commit(), performed by
 *     persist_table() below. Every mutating call (plants_resolve_or_create,
 *     plants_rename, plants_assign, plants_create, plants_delete) follows
 *     the same shape: PHASE 1 (RAM, under s_mutex only) mutates s_table and
 *     releases s_mutex completely; PHASE 2 (persist_table()) takes
 *     s_nvs_mutex, briefly RE-takes s_mutex just long enough to copy the
 *     CURRENT s_table -- not whatever phase 1 computed, which may already
 *     be stale by the time phase 2 actually runs -- releases s_mutex,
 *     writes the blob, then releases s_nvs_mutex. This closes the same two
 *     gaps swarm_store.c's comment documents: a reader can never
 *     transitively wait on another writer's in-flight commit, and two
 *     concurrent writers' commits can never land out of order relative to
 *     RAM freshness (whichever one actually holds s_nvs_mutex at write time
 *     always writes the freshest RAM state, so flash only ever moves
 *     forward).
 *
 *   - Consequence, same as swarm_store.c: the RAM cache can run ahead of
 *     flash if a commit fails (logged, never silently swallowed) -- it
 *     remains the source of truth for the rest of THIS boot regardless. A
 *     reboot before a failed write's next retry reverts to whatever last
 *     actually committed.
 *
 * plants_resolve_or_create() in particular is documented as TASK CONTEXT
 * ONLY (plants.h) -- unlike swarm_store's node lookups it is never called
 * from an ISR or a must-not-block callback, but it still sits on the
 * sampler/BLE ingestion path, so keeping its NVS write off the RAM mutex
 * (via the same two-phase split) still matters: a concurrent HTTP-triggered
 * rename's flash commit must never stall the next sensor sample's mac
 * resolution. */
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_nvs_mutex;
static plants_table_t s_table;
static const char *s_storage_base;   /* "/storage", or NULL: see plants_init() */

/* plants_last_values() RAM cache -- guarded by its own mutex, deliberately
 * separate from s_mutex/s_nvs_mutex above: it is populated by a
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

static esp_err_t write_blob(const plants_blob_t *blob)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, KEY_PLANTS, blob, sizeof(*blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Phase-2 persistence (see the locking invariant comment above): serialises
 * against other writers via s_nvs_mutex, re-reads the CURRENT s_table under
 * a brief s_mutex hold (never held during the flash step itself), packs it
 * into the on-disk mirror shape, then writes it. Callers must have already
 * completed their own RAM mutation under s_mutex and released it BEFORE
 * calling this -- it must never be called while still holding s_mutex. */
static esp_err_t persist_table(void)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    plants_table_t snapshot = s_table;
    xSemaphoreGive(s_mutex);

    plants_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.format = PLANTS_BLOB_FORMAT;
    blob.next_id = snapshot.next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        blob.p[i].id = snapshot.p[i].id;
        blob.p[i].in_use = snapshot.p[i].in_use ? 1 : 0;
        memcpy(blob.p[i].mac, snapshot.p[i].mac, 6);
        blob.p[i].mac_valid = snapshot.p[i].mac_valid ? 1 : 0;
        memcpy(blob.p[i].name, snapshot.p[i].name, sizeof(blob.p[i].name));
    }

    esp_err_t err = write_blob(&blob);
    xSemaphoreGive(s_nvs_mutex);
    return err;
}

/* Loads the KEY_PLANTS blob into *out, which the caller has already reset
 * to plants_table_init() defaults -- every return path below (including
 * every failure) therefore leaves *out valid. Wrong length or an
 * unrecognised format byte is loudly logged and left at those defaults
 * (empty table) rather than trusting a shape that cannot be relied on --
 * same contract as swarm_store.c's load_nodes_blob() for its unrecognised
 * cases, minus the migration branches (nothing to migrate from yet: this is
 * the first on-disk format for plants). */
static void load_blob(nvs_handle_t h, plants_table_t *out)
{
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, KEY_PLANTS, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;  /* fresh install: no plant table yet */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "plants blob length query failed: %s; starting with an empty plant table",
                 esp_err_to_name(err));
        return;
    }
    if (len != sizeof(plants_blob_t)) {
        ESP_LOGW(TAG, "plants blob has unrecognised length %d (expected %d); starting with an "
                      "empty plant table", (int)len, (int)sizeof(plants_blob_t));
        return;
    }

    plants_blob_t blob;
    size_t rlen = sizeof(blob);
    if (nvs_get_blob(h, KEY_PLANTS, &blob, &rlen) != ESP_OK || rlen != sizeof(blob)) {
        ESP_LOGW(TAG, "plants blob read failed at its expected length; starting with an empty "
                      "plant table");
        return;
    }
    if (blob.format != PLANTS_BLOB_FORMAT) {
        ESP_LOGE(TAG, "plants blob has unknown format byte %u (expected %u) at the expected "
                      "length -- DISCARDING it rather than trusting an unrecognised layout; "
                      "every plant is lost until re-created",
                 blob.format, (unsigned)PLANTS_BLOB_FORMAT);
        return;
    }

    plants_table_t t;
    t.next_id = blob.next_id;
    for (int i = 0; i < PLANTS_MAX; i++) {
        t.p[i].in_use = blob.p[i].in_use != 0;
        t.p[i].id = blob.p[i].id;
        t.p[i].mac_valid = blob.p[i].mac_valid != 0;
        memcpy(t.p[i].mac, blob.p[i].mac, 6);
        memcpy(t.p[i].name, blob.p[i].name, sizeof(t.p[i].name));
        t.p[i].name[PLANT_NAME_LEN] = '\0';  /* defensive: guard against a corrupt non-terminated blob */
    }
    *out = t;
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
 * here -- this function only touches the filesystem and NVS. */
static void migrate_execute_action(const migrate_action_t *a)
{
    char mac_hex[13];
    snprintf(mac_hex, sizeof(mac_hex), "%02X%02X%02X%02X%02X%02X",
             a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);

    if (a->rename_raw && s_storage_base) {
        char oldp[128], newp[128];
        snprintf(oldp, sizeof(oldp), "%s/%s_raw.bin", s_storage_base, mac_hex);
        snprintf(newp, sizeof(newp), "%s/P%u_raw.bin", s_storage_base, a->plant_id);
        migrate_rename_one(oldp, newp);
    }
    if (a->rename_hourly && s_storage_base) {
        char oldp[128], newp[128];
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
 * below. s_table already holds whatever load_blob() found (normally
 * empty; plants_migrate_plan() skips any mac already in it either way).
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
     * lock only needs to span this one call, not the file/NVS work below. */
    migrate_action_t actions[PLANTS_MAX];
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

esp_err_t plants_init(const char *storage_base)
{
    s_mutex = xSemaphoreCreateMutex();
    s_nvs_mutex = xSemaphoreCreateMutex();
    s_lv_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || !s_nvs_mutex || !s_lv_mutex) return ESP_ERR_NO_MEM;

    s_storage_base = storage_base;
    memset(s_full_logged, 0, sizeof(s_full_logged));
    memset(s_last_values, 0, sizeof(s_last_values));
    plants_table_init(&s_table);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_blob(h, &s_table);
        nvs_close(h);
    }
    /* nvs_open() failure here means a fresh "planthub" namespace (nothing
     * has ever been written to it) -- s_table already holds
     * plants_table_init()'s empty defaults, same as swarm_store_init()'s
     * "fresh NVS = defaults" fallback. */

    plants_run_migration();

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
        ESP_LOGE(TAG, "plants_resolve_or_create: NVS write failed (%s); RAM cache already "
                      "reflects the new plant and will not revert until the next successful "
                      "write or a reboot", esp_err_to_name(err));
    }
    return new_id;
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
        ESP_LOGE(TAG, "plants_rename(%u): NVS write failed (%s); RAM cache already reflects "
                      "the new name and will not revert until the next successful write or a "
                      "reboot", id, esp_err_to_name(err));
    }
    return err;
}

esp_err_t plants_assign(uint8_t id, const uint8_t *mac_or_null)
{
    /* Same uninitialised-registry guard as plants_rename() above. */
    if (!s_mutex) return ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = plants_table_assign(&s_table, id, mac_or_null);
    xSemaphoreGive(s_mutex);
    if (!ok) return ESP_ERR_NOT_FOUND;

    esp_err_t err = persist_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "plants_assign(%u): NVS write failed (%s); RAM cache already reflects "
                      "the new assignment and will not revert until the next successful write "
                      "or a reboot", id, esp_err_to_name(err));
    }
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
        ESP_LOGE(TAG, "plants_create: NVS write failed (%s); RAM cache already reflects the "
                      "new plant and will not revert until the next successful write or a "
                      "reboot", esp_err_to_name(err));
    }
    return id;
}

/* plants_last_values() cache helpers -- see s_last_values' declaration
 * comment for the slot-capacity argument. */
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
        ESP_LOGE(TAG, "plants_delete(%u): NVS write failed (%s); RAM cache already reflects "
                      "the deletion and will not revert until the next successful write or a "
                      "reboot", id, esp_err_to_name(err));
    }

    remove_ring_files(id);
    /* plants_last_values()'s RAM cache is dropped here too: ids are never
     * reused (plants_table.h), so this id will never be looked up again as
     * a live plant, but a stray caller holding onto a stale id must still
     * see "no history" rather than whatever was cached before the delete. */
    lv_cache_drop(id);
    return err;
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
    bool          found;
    uint32_t      epoch;
    storage_rec_t rec;
} lv_scan_ctx_t;

/* storage_query() emits rows in ring order, strictly oldest-to-newest (see
 * storage.c and test_storage.c's wraparound-order assertions) -- so simply
 * overwriting on every call, with no comparison, leaves the newest row in
 * *ctx once the scan completes. */
static void lv_row(void *vctx, uint32_t epoch, const storage_rec_t *rec)
{
    lv_scan_ctx_t *c = vctx;
    c->found = true;
    c->epoch = epoch;
    c->rec = *rec;
}

bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,
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
        last_values_cache_t c = s_last_values[idx];
        xSemaphoreGive(s_lv_mutex);
        if (!c.found) return false;
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
        storage_query(s_storage_base, id, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu,
                      lv_resolve, NULL, lv_row, &scan);
    }

    /* Cache the result -- found or not -- keyed by id, so a plant that
     * genuinely has no history (e.g. never had a probe assigned) doesn't
     * re-scan its non-existent ring file on every call. Per plants.h, this
     * is deliberately a first-read-wins cache with no periodic refresh:
     * plants_last_values() exists to serve probe-less plants (see plants.h),
     * whose ring, once populated, only ever changes via plants_delete()
     * (which drops this cache entry too, above). */
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
        c->found = scan.found;
        if (scan.found) {
            c->temp_dc = scan.rec.temp_dc;
            c->moisture_pct = scan.rec.moisture_pct;
            c->lux = scan.rec.lux;
            c->conductivity_us = scan.rec.conductivity_us;
            c->battery_pct = scan.rec.battery_pct;
            c->epoch = scan.epoch;
        }
    } else {
        /* Cache exhausted -- cannot happen in practice (capacity ==
         * PLANTS_MAX and plants_delete() always frees its slot), but if it
         * ever does this call still answers correctly, it just re-scans
         * storage on every future call for this id instead of caching. */
        ESP_LOGW(TAG, "plants_last_values(%u): last-values cache full; not caching this result", id);
    }
    xSemaphoreGive(s_lv_mutex);

    if (!scan.found) return false;
    if (temp_dc)       *temp_dc = scan.rec.temp_dc;
    if (moisture)      *moisture = scan.rec.moisture_pct;
    if (lux)           *lux = scan.rec.lux;
    if (conductivity)  *conductivity = scan.rec.conductivity_us;
    if (battery)       *battery = scan.rec.battery_pct;
    if (epoch_out)      *epoch_out = scan.epoch;
    return true;
}
