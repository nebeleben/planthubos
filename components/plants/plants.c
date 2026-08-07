#include "plants.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

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

esp_err_t plants_init(const char *storage_base)
{
    s_mutex = xSemaphoreCreateMutex();
    s_nvs_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || !s_nvs_mutex) return ESP_ERR_NO_MEM;

    s_storage_base = storage_base;
    memset(s_full_logged, 0, sizeof(s_full_logged));
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

    /* Task 4: the one-boot plant migration hook lands here (no-op until
     * then; plants_migrate.c is currently a stub). */

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
    if (!mac) return 0;

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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_table;
    xSemaphoreGive(s_mutex);
}

esp_err_t plants_rename(uint8_t id, const char *name)
{
    if (!name || strlen(name) > PLANT_NAME_LEN) return ESP_ERR_INVALID_ARG;

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

/* Removes P<id>_raw.bin/P<id>_hourly.bin under the base path recorded at
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
    snprintf(path, sizeof(path), "%s/P%u_hourly.bin", s_storage_base, id);
    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "plants_delete(%u): failed to remove %s: %s", id, path, strerror(errno));
    }
}

esp_err_t plants_delete(uint8_t id)
{
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
    /* plants_last_values()'s RAM cache (once Task 3 adds one) is
     * invalidated here too -- moot today since that function is stubbed to
     * always return false and caches nothing (see plants.h). */
    return err;
}

bool plants_last_values(uint8_t id, int16_t *temp_dc, uint8_t *moisture,
                        uint32_t *lux, uint16_t *conductivity, uint8_t *battery,
                        uint32_t *epoch_out)
{
    /* TASK 3 REKEYS THIS: switch to plant_id -- see plants.h for why this
     * is stubbed rather than querying storage.c by the plant's assigned mac
     * today. Outputs deliberately left untouched, same "false = untouched"
     * contract as swarm_store_hub() etc. */
    (void)id;
    (void)temp_dc;
    (void)moisture;
    (void)lux;
    (void)conductivity;
    (void)battery;
    (void)epoch_out;
    return false;
}
