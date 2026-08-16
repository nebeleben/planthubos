/* wrapper_store.c -- LittleFS-backed wrapper store (M3 spec §2 "Matcher and
 * wrapper store"): one wrapper lives as three files under
 * /storage/wrappers/, `<id>.wsrc` (source text), `<id>.wbc` (bytecode) and
 * `<id>.json` (meta: name, enabled, match_kind, match_key -- Task 7 is the
 * one that writes these; this task only reads them). id = u16 monotonic,
 * never reused -- same invariant components/rules/rules_store.c documents
 * for rule ids and plants_table.h documents for plant ids.
 *
 * Follows rules_store.c's idioms closely (house pattern, reviewed hard in
 * M1): same file-IO helpers (path_for/read_whole_file/parse_id_from_json_name
 * lifted near-verbatim), same "one bad file logs and is skipped, never fails
 * boot" tolerance, same id = max-existing + 1 rule. Two differences, both
 * because this task's scope stops short of a full store:
 *   - No write_atomic()/upsert here -- Task 7 owns the wrapper CRUD API and
 *     adds the write side to this file then. Task 2 only builds the boot-
 *     time read path the decoder needs today.
 *   - Nothing here keeps a resident per-wrapper metadata table (rules_store.c's
 *     g_rules) -- a wrapper's name/enabled/etc. aren't needed by anything in
 *     M3 Task 2 (the decoder only needs the match index), and RAM is the
 *     scarce resource this milestone is watching (spec §7). Task 7 adds
 *     whatever metadata table its list/get endpoints need.
 */
#include "wrapper_index.h"
#include "cJSON.h"
#include "esp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "wrappers";

#define WRAPPERS_STORAGE_DIR "/storage/wrappers"
#define WRAPPER_META_MAX     256   /* generous headroom over a 48-char name + 2 numbers + JSON syntax */

/* Next id to hand out on a future create (Task 7). Computed here at boot,
 * same as rules_store.c's g_rules_next_id, but file-scope only: nothing in
 * this task creates a wrapper, so nothing outside this file needs it yet. */
static uint16_t s_wrapper_next_id = 1;

static bool path_for(uint32_t id, const char *ext, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "%s/%u.%s", WRAPPERS_STORAGE_DIR, (unsigned)id, ext);
    return n > 0 && (size_t)n < buflen;
}

/* Reads the whole file into buf (capacity buflen); rejects (returns false) a
 * file that turns out to be longer than buflen rather than silently
 * truncating it -- same "confirm the file isn't LONGER than expected"
 * pattern as rules_store.c's/plants.c's read_whole_file()/load_file().
 *
 * On that "too long for buflen" failure specifically, *len_out is still set
 * to the file's TRUE size (one extra fseek/ftell, cheap relative to the
 * already-failed read) rather than left untouched -- M3 Task 5's
 * wrapper_arena.c (the only caller that ever passes a `buflen` smaller than
 * a blob might genuinely be) uses this to tell "would fit after evicting N
 * bytes from the arena" from "will never fit at all, refuse without
 * evicting anything" without a second stat() of its own. Every EXISTING
 * caller of this helper (load_one()'s meta-file reads, just below) already
 * discards len_out entirely on a false return, so this changes no observed
 * behaviour for them. A genuine I/O failure (fopen itself failing) leaves
 * *len_out untouched, same as before -- there is no "true size" to report
 * for a file that couldn't even be opened. */
static bool read_whole_file(const char *path, void *buf, size_t buflen, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, buflen, f);
    int extra = fgetc(f);
    if (extra != EOF) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (len_out && sz > 0) *len_out = (size_t)sz;
        return false;
    }
    fclose(f);
    if (len_out) *len_out = n;
    return true;
}

/* True + *id_out set iff name is exactly "<digits>.json" (id >= 1, ids are
 * 1-based) -- wrapper_store_load_all()'s directory scan uses this to
 * recognise a wrapper's meta file among whatever else might be in the
 * directory. Lifted from rules_store.c's parse_id_from_json_name(). */
static bool parse_id_from_json_name(const char *name, uint32_t *id_out)
{
    static const char suffix[] = ".json";
    size_t len = strlen(name);
    size_t suflen = sizeof(suffix) - 1;
    if (len <= suflen || strcmp(name + len - suflen, suffix) != 0) return false;
    size_t idlen = len - suflen;
    if (idlen == 0 || idlen > 10) return false;

    uint32_t id = 0;
    for (size_t i = 0; i < idlen; i++) {
        char c = name[i];
        if (c < '0' || c > '9') return false;
        id = id * 10 + (uint32_t)(c - '0');
    }
    if (id == 0) return false;
    *id_out = id;
    return true;
}

/* Parses one wrapper's meta JSON and, if it is enabled and its match key
 * indexes cleanly, adds it to *ix. Missing/corrupt meta, a disabled
 * wrapper, an out-of-range match_kind, or wrapper_index_add() rejecting the
 * key (table full or a colliding (kind,key) pair -- both already logged by
 * the caller below) all just skip this one wrapper, logged, same tolerance
 * rules_store.c's load_one() has for a bad rule. */
static void load_one(uint32_t id, wrapper_index_t *ix)
{
    char path[160];
    if (!path_for(id, "json", path, sizeof(path))) return;

    char jbuf[WRAPPER_META_MAX];
    size_t jlen;
    if (!read_whole_file(path, jbuf, sizeof(jbuf) - 1, &jlen)) {
        ESP_LOGW(TAG, "wrapper %u: meta file unreadable or too large; skipping", (unsigned)id);
        return;
    }
    jbuf[jlen] = '\0';

    cJSON *root = cJSON_ParseWithLength(jbuf, jlen);
    if (!root) {
        ESP_LOGW(TAG, "wrapper %u: meta file is not valid JSON; skipping", (unsigned)id);
        return;
    }
    cJSON *jenabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *jkind    = cJSON_GetObjectItemCaseSensitive(root, "match_kind");
    cJSON *jkey     = cJSON_GetObjectItemCaseSensitive(root, "match_key");
    if (!cJSON_IsBool(jenabled) || !cJSON_IsNumber(jkind) || !cJSON_IsNumber(jkey)) {
        ESP_LOGW(TAG, "wrapper %u: meta file missing/invalid fields; skipping", (unsigned)id);
        cJSON_Delete(root);
        return;
    }
    bool enabled = cJSON_IsTrue(jenabled);
    double kind_d = jkind->valuedouble;
    double key_d = jkey->valuedouble;
    cJSON_Delete(root);

    if (!enabled) {
        ESP_LOGI(TAG, "wrapper %u: disabled; not indexed", (unsigned)id);
        return;
    }
    if (kind_d < WMATCH_SERVICE || kind_d > WMATCH_MAC_PREFIX) {
        ESP_LOGW(TAG, "wrapper %u: bad match_kind %d in meta; skipping", (unsigned)id, (int)kind_d);
        return;
    }
    if (wrapper_index_add(ix, (uint8_t)kind_d, (uint32_t)key_d, (uint16_t)id) != 0) {
        ESP_LOGW(TAG, "wrapper %u: not indexed (table full or a duplicate match key); skipping", (unsigned)id);
    }
}

void wrapper_store_load_all(wrapper_index_t *ix)
{
    wrapper_index_init(ix);
    s_wrapper_next_id = 1;

    if (mkdir(WRAPPERS_STORAGE_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s -- wrappers will not persist", WRAPPERS_STORAGE_DIR, strerror(errno));
    }

    DIR *d = opendir(WRAPPERS_STORAGE_DIR);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed: %s -- starting with no wrappers", WRAPPERS_STORAGE_DIR, strerror(errno));
        return;
    }
    uint32_t max_id = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint32_t id;
        if (!parse_id_from_json_name(de->d_name, &id)) continue;
        if (id > max_id) max_id = id;   /* id stays retired even if indexing below skips it -- never reused */
        load_one(id, ix);
    }
    closedir(d);
    s_wrapper_next_id = (uint16_t)(max_id + 1);

    ESP_LOGI(TAG, "wrapper_store_load_all: %u wrapper(s) indexed, next_id=%u",
             (unsigned)ix->count, (unsigned)s_wrapper_next_id);
}

bool wrapper_store_read_psbc(uint16_t id, uint8_t *buf, size_t cap, size_t *len_out)
{
    char path[160];
    if (!path_for(id, "wbc", path, sizeof(path))) return false;
    return read_whole_file(path, buf, cap, len_out);
}
