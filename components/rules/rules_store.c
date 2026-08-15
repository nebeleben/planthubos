/* rules_store.c -- LittleFS-backed rule store (spec §4 "Store"): one rule
 * lives as three files under /storage/rules/, `<id>.psrc` (source text),
 * `<id>.psbc` (bytecode) and `<id>.json` (meta: name, enabled, mode,
 * cooldown_s, every_s -- brief step 1's exact field list). id = u32
 * monotonic, never reused (same invariant plants_table.h documents for
 * plant ids). Also owns g_rules, the shared in-RAM metadata table
 * rules_engine.c/rules_resolver.c read.
 *
 * File-IO idiom mirrors components/plants/plants.c: every write goes to a
 * sibling ".tmp" file first (fwrite+fflush+fclose), then rename()s it over
 * the real path -- rename() is atomic on LittleFS, so a reader or a power
 * loss only ever sees the old complete file or the new complete one, never
 * a partial write. */
#include "rules_internal.h"
#include "cJSON.h"
#include "esp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "rules";

#define RULES_META_MAX 512   /* generous headroom over a 48-char name + 4 numbers + JSON syntax */

rule_t            g_rules[RULES_MAX];
SemaphoreHandle_t g_rules_mutex;
uint32_t          g_rules_next_id = 1;

static bool path_for(uint32_t id, const char *ext, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "%s/%u.%s", RULES_STORAGE_DIR, (unsigned)id, ext);
    return n > 0 && (size_t)n < buflen;
}

static bool write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[160];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (ok) ok = (fflush(f) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGE(TAG, "write %s failed: %s", tmp, strerror(errno));
        remove(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: %s", tmp, path, strerror(errno));
        remove(tmp);
        return false;
    }
    return true;
}

/* Reads the whole file into buf (capacity buflen); rejects (returns false)
 * a file that turns out to be longer than buflen rather than silently
 * truncating it -- same "confirm the file isn't LONGER than expected"
 * pattern as plants.c's load_file(). *len_out is the number of bytes
 * actually read (never includes a NUL -- callers that want a C string add
 * their own, see rules_get_source()). */
static bool read_whole_file(const char *path, void *buf, size_t buflen, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, buflen, f);
    int extra = fgetc(f);
    fclose(f);
    if (extra != EOF) return false;
    if (len_out) *len_out = n;
    return true;
}

static const char *psvm_err_str(psvm_err_t e)
{
    switch (e) {
    case PSVM_OK:            return "ok";
    case PSVM_ERR_HEADER:    return "bad bytecode header (magic/version/dialect/flags)";
    case PSVM_ERR_LIMITS:    return "bytecode exceeds a hub limit or uses an unimplemented builtin/capability";
    case PSVM_ERR_TRUNCATED: return "bytecode truncated";
    case PSVM_ERR_BADOP:     return "bad opcode";
    case PSVM_ERR_STACK:     return "stack error";
    case PSVM_ERR_STEPS:     return "step budget exceeded";
    case PSVM_ERR_DIV0:      return "division by zero";
    case PSVM_ERR_JUMP:      return "bad jump target";
    case PSVM_ERR_TYPE:      return "type error";
    case PSVM_ERR_REF:       return "bad reference (unknown capability, or ref bounds)";
    default:                 return "unknown bytecode error";
    }
}

static bool save_meta(const rule_t *r)
{
    char path[160];
    if (!path_for(r->id, "json", path, sizeof(path))) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    bool built = cJSON_AddStringToObject(root, "name", r->name) != NULL &&
                cJSON_AddBoolToObject(root, "enabled", r->enabled) != NULL &&
                cJSON_AddNumberToObject(root, "mode", r->mode) != NULL &&
                cJSON_AddNumberToObject(root, "cooldown_s", r->cooldown_s) != NULL &&
                cJSON_AddNumberToObject(root, "every_s", r->every_s) != NULL;
    if (!built) { cJSON_Delete(root); return false; }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;
    bool ok = write_atomic(path, txt, strlen(txt));
    cJSON_free(txt);
    return ok;
}

/* True + *id_out set iff name is exactly "<digits>.json" (id >= 1, ids are
 * 1-based -- rules_store_load_all()'s directory scan uses this to recognise
 * a rule's meta file among whatever else might be in the directory). */
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

/* Loads one rule's meta JSON + stats its source/bytecode file sizes into a
 * fresh g_rules slot. Missing/corrupt source or bytecode files do not drop
 * the rule -- source_len/psbc_len simply read 0 and the engine's first
 * evaluation reports not-ready (psvm_validate() on an empty read) rather
 * than this function failing boot. Only a genuinely bad/missing meta file
 * (or a full RAM table) skips the rule outright. */
static void load_one(uint32_t id)
{
    char path[160];
    if (!path_for(id, "json", path, sizeof(path))) return;

    char jbuf[RULES_META_MAX];
    size_t jlen;
    if (!read_whole_file(path, jbuf, sizeof(jbuf) - 1, &jlen)) {
        ESP_LOGW(TAG, "rule %u: meta file unreadable or too large; skipping", (unsigned)id);
        return;
    }
    jbuf[jlen] = '\0';

    cJSON *root = cJSON_ParseWithLength(jbuf, jlen);
    if (!root) {
        ESP_LOGW(TAG, "rule %u: meta file is not valid JSON; skipping", (unsigned)id);
        return;
    }
    cJSON *jname     = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *jenabled  = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *jmode     = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *jcooldown = cJSON_GetObjectItemCaseSensitive(root, "cooldown_s");
    cJSON *jevery    = cJSON_GetObjectItemCaseSensitive(root, "every_s");
    if (!cJSON_IsString(jname) || !cJSON_IsBool(jenabled) || !cJSON_IsNumber(jmode) ||
        !cJSON_IsNumber(jcooldown) || !cJSON_IsNumber(jevery)) {
        ESP_LOGW(TAG, "rule %u: meta file missing/invalid fields; skipping", (unsigned)id);
        cJSON_Delete(root);
        return;
    }

    int slot = -1;
    for (int i = 0; i < RULES_MAX; i++) {
        if (!g_rules[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "rule %u: RAM table full (RULES_MAX=%d); skipping", (unsigned)id, RULES_MAX);
        cJSON_Delete(root);
        return;
    }

    rule_t *r = &g_rules[slot];
    memset(r, 0, sizeof(*r));
    r->in_use = true;
    r->id = id;
    strlcpy(r->name, cJSON_GetStringValue(jname), sizeof(r->name));
    r->enabled = cJSON_IsTrue(jenabled);
    r->mode = (uint8_t)jmode->valuedouble;
    r->cooldown_s = (uint32_t)jcooldown->valuedouble;
    r->every_s = (uint32_t)jevery->valuedouble;
    cJSON_Delete(root);

    rules_fsm_reset(&r->fsm);
    r->ready = false;
    strlcpy(r->not_ready_reason, "not yet evaluated", sizeof(r->not_ready_reason));
    r->last_err = PSVM_OK;

    struct stat st;
    char p2[160];
    if (path_for(id, "psrc", p2, sizeof(p2)) && stat(p2, &st) == 0) {
        r->source_len = (size_t)st.st_size;
    } else {
        ESP_LOGW(TAG, "rule %u: missing/unreadable %s", (unsigned)id, p2);
    }
    if (path_for(id, "psbc", p2, sizeof(p2)) && stat(p2, &st) == 0) {
        r->psbc_len = (size_t)st.st_size;
    } else {
        ESP_LOGW(TAG, "rule %u: missing/unreadable %s", (unsigned)id, p2);
    }

    rules_engine_sync_timer(r);
}

void rules_store_load_all(void)
{
    g_rules_mutex = xSemaphoreCreateMutex();
    if (!g_rules_mutex) {
        ESP_LOGE(TAG, "failed to create rules mutex; rules engine disabled");
        return;
    }
    memset(g_rules, 0, sizeof(g_rules));
    g_rules_next_id = 1;

    if (mkdir(RULES_STORAGE_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s -- rules will not persist", RULES_STORAGE_DIR, strerror(errno));
    }

    DIR *d = opendir(RULES_STORAGE_DIR);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed: %s -- starting with no rules", RULES_STORAGE_DIR, strerror(errno));
        return;
    }
    uint32_t max_id = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint32_t id;
        if (!parse_id_from_json_name(de->d_name, &id)) continue;
        if (id > max_id) max_id = id;   /* id stays retired even if the load below fails -- never reused */
        load_one(id);
    }
    closedir(d);
    g_rules_next_id = max_id + 1;

    int count = 0;
    for (int i = 0; i < RULES_MAX; i++) {
        if (g_rules[i].in_use) count++;
    }
    ESP_LOGI(TAG, "rules_store_load_all: %d rule(s) loaded, next_id=%u", count, (unsigned)g_rules_next_id);
}

bool rules_store_read_psbc(uint32_t id, uint8_t *buf, size_t buflen, size_t *len_out)
{
    char path[160];
    if (!path_for(id, "psbc", path, sizeof(path))) return false;
    return read_whole_file(path, buf, buflen, len_out);
}

size_t rules_list(rule_info_t *out, size_t max)
{
    if (!out || max == 0 || !g_rules_mutex) return 0;
    size_t n = 0;
    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    for (int i = 0; i < RULES_MAX && n < max; i++) {
        if (!g_rules[i].in_use) continue;
        const rule_t *r = &g_rules[i];
        rule_info_t *o = &out[n++];
        o->id = r->id;
        strlcpy(o->name, r->name, sizeof(o->name));
        o->enabled = r->enabled;
        o->mode = r->mode;
        o->cooldown_s = r->cooldown_s;
        o->every_s = r->every_s;
        o->ready = r->ready;
        strlcpy(o->not_ready_reason, r->not_ready_reason, sizeof(o->not_ready_reason));
        o->last_err = r->last_err;
        o->last_eval_ts = r->last_eval_ts;
        o->last_fire_ts = r->last_fire_ts;
        o->fire_count = r->fire_count;
    }
    xSemaphoreGive(g_rules_mutex);
    return n;
}

bool rules_get_source(uint32_t id, char *buf, size_t buflen)
{
    if (!buf || buflen == 0 || !g_rules_mutex) return false;

    bool known = false;
    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    for (int i = 0; i < RULES_MAX; i++) {
        if (g_rules[i].in_use && g_rules[i].id == id) { known = true; break; }
    }
    xSemaphoreGive(g_rules_mutex);
    if (!known) return false;

    char path[160];
    if (!path_for(id, "psrc", path, sizeof(path))) return false;
    size_t len;
    if (!read_whole_file(path, buf, buflen - 1, &len)) return false;
    buf[len] = '\0';
    return true;
}

static void seterr(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen) snprintf(errbuf, errlen, "%s", msg);
}

int rules_upsert(uint32_t *id_inout, const char *name, const char *source,
                 const uint8_t *psbc, size_t psbc_len, bool enabled,
                 uint8_t mode, uint32_t cooldown_s, uint32_t every_s,
                 char *errbuf, size_t errlen)
{
    if (errbuf && errlen) errbuf[0] = '\0';
    if (!id_inout || !name || !source || !psbc) {
        seterr(errbuf, errlen, "missing argument");
        return ESP_ERR_INVALID_ARG;
    }

    /* Meta/size limits (spec §1 name grammar, §4 store limits) -- checked
     * before touching the store or the VM at all. */
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > RULES_NAME_MAX || strchr(name, '"') != NULL) {
        seterr(errbuf, errlen, "name must be 1-48 characters and must not contain '\"'");
        return ESP_ERR_INVALID_ARG;
    }
    size_t srclen = strlen(source);
    if (srclen == 0 || srclen > RULES_SRC_MAX) {
        seterr(errbuf, errlen, "source must be 1-4096 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    if (psbc_len == 0 || psbc_len > RULES_PSBC_MAX) {
        seterr(errbuf, errlen, "bytecode must be 1-2048 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != RULES_MODE_EDGE && mode != RULES_MODE_LEVEL) {
        seterr(errbuf, errlen, "mode must be 0 (edge) or 1 (level)");
        return ESP_ERR_INVALID_ARG;
    }
    if (every_s != 0 && (every_s < 30 || every_s > 86400)) {
        seterr(errbuf, errlen, "every must be 0 (disabled) or between 30s and 24h");
        return ESP_ERR_INVALID_ARG;
    }

    /* Bytecode itself: header/limits/refs bounds (spec §2's forward-
     * compatibility gate) -- reject unknown capability ids / builtins here,
     * at upload, not at first evaluation. */
    psvm_prog_t prog;
    psvm_err_t verr = psvm_validate(psbc, psbc_len, RULES_CAP_MAX_ID, RULES_BUILTINS_IMPL, &prog);
    if (verr != PSVM_OK) {
        seterr(errbuf, errlen, psvm_err_str(verr));
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_rules_mutex) {
        seterr(errbuf, errlen, "rules store not initialised");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    bool is_new = (*id_inout == 0);
    int idx = -1;
    uint32_t id;
    if (is_new) {
        for (int i = 0; i < RULES_MAX; i++) {
            if (!g_rules[i].in_use) { idx = i; break; }
        }
        if (idx < 0) {
            xSemaphoreGive(g_rules_mutex);
            seterr(errbuf, errlen, "rule table full (16 rules max)");
            return ESP_ERR_NO_MEM;
        }
        id = g_rules_next_id++;
    } else {
        for (int i = 0; i < RULES_MAX; i++) {
            if (g_rules[i].in_use && g_rules[i].id == *id_inout) { idx = i; break; }
        }
        if (idx < 0) {
            xSemaphoreGive(g_rules_mutex);
            seterr(errbuf, errlen, "unknown rule id");
            return ESP_ERR_INVALID_ARG;
        }
        id = *id_inout;
    }

    rule_t *r = &g_rules[idx];
    if (is_new) memset(r, 0, sizeof(*r));
    r->in_use = true;
    r->id = id;
    strlcpy(r->name, name, sizeof(r->name));
    r->enabled = enabled;
    r->mode = mode;
    r->cooldown_s = cooldown_s;
    r->every_s = every_s;
    r->source_len = srclen;
    r->psbc_len = psbc_len;
    /* The compiled logic just changed (create, or an edit on update) -- the
     * edge-armed/last-fire history from before no longer describes this
     * rule's condition, so re-arm rather than carry it forward. fire_count
     * is treated as a lifetime stat, not part of that state, and survives an
     * update (only rules_delete() clears it, by dropping the whole entry). */
    rules_fsm_reset(&r->fsm);
    r->ready = false;
    strlcpy(r->not_ready_reason, "not yet evaluated", sizeof(r->not_ready_reason));
    r->last_err = PSVM_OK;
    r->last_eval_ts = 0;
    if (is_new) {
        r->last_fire_ts = 0;
        r->fire_count = 0;
    }
    rules_engine_sync_timer(r);
    rule_t snapshot = *r;
    xSemaphoreGive(g_rules_mutex);

    char path[160];
    bool ok = path_for(id, "psrc", path, sizeof(path)) && write_atomic(path, source, srclen);
    if (ok) ok = path_for(id, "psbc", path, sizeof(path)) && write_atomic(path, psbc, psbc_len);
    if (ok) ok = save_meta(&snapshot);

    *id_inout = id;
    if (!ok) {
        /* RAM already reflects the change and will not revert (same "RAM
         * can run ahead of a failed write" contract as plants.c's
         * mutators) -- id/slot stay claimed; a retry with the same id
         * re-attempts the writes. */
        seterr(errbuf, errlen, "failed to persist rule to storage");
        ESP_LOGE(TAG, "rule %u: failed to persist to %s", (unsigned)id, RULES_STORAGE_DIR);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "rule %u (\"%s\") %s", (unsigned)id, snapshot.name, is_new ? "created" : "updated");
    return ESP_OK;
}

bool rules_delete(uint32_t id)
{
    if (!g_rules_mutex) return false;

    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < RULES_MAX; i++) {
        if (g_rules[i].in_use && g_rules[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(g_rules_mutex);
        return false;
    }
    rule_t *r = &g_rules[idx];
    r->every_s = 0;
    rules_engine_sync_timer(r);   /* tears down any periodic timer before the slot is wiped */
    memset(r, 0, sizeof(*r));     /* in_use=false; slot free for reuse -- id itself never is */
    xSemaphoreGive(g_rules_mutex);

    char path[160];
    if (path_for(id, "psrc", path, sizeof(path))) remove(path);
    if (path_for(id, "psbc", path, sizeof(path))) remove(path);
    if (path_for(id, "json", path, sizeof(path))) remove(path);
    ESP_LOGI(TAG, "rule %u deleted", (unsigned)id);
    return true;
}

bool rules_set_enabled(uint32_t id, bool enabled)
{
    if (!g_rules_mutex) return false;

    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < RULES_MAX; i++) {
        if (g_rules[i].in_use && g_rules[i].id == id) { idx = i; break; }
    }
    bool changed = (idx >= 0) && (g_rules[idx].enabled != enabled);
    if (idx >= 0) {
        g_rules[idx].enabled = enabled;
        if (changed && enabled) {
            /* Re-enabling re-arms from scratch (review fix): a rule
             * disabled while armed=false (already fired, condition still
             * true) must not skip straight back to firing the instant it's
             * re-enabled with no transition -- evaluate_all() never touches
             * a disabled rule's fsm, so without this its armed/
             * ever_evaluated state would just be whatever it was the
             * moment it got disabled. */
            rules_fsm_reset(&g_rules[idx].fsm);
        }
    }
    rule_t snapshot;
    if (changed) snapshot = g_rules[idx];
    xSemaphoreGive(g_rules_mutex);

    if (idx < 0) return false;
    if (changed && !save_meta(&snapshot)) {
        ESP_LOGE(TAG, "rule %u: failed to persist enabled=%d", (unsigned)id, (int)enabled);
    }
    return true;
}
