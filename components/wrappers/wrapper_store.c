/* wrapper_store.c -- LittleFS-backed wrapper store (M3 spec §2 "Matcher and
 * wrapper store"): one wrapper lives as three files under
 * /storage/wrappers/, `<id>.wsrc` (source text), `<id>.wbc` (bytecode) and
 * `<id>.json` (meta: name, enabled, match_kind, match_key). id = u16
 * monotonic, never reused -- same invariant components/rules/rules_store.c
 * documents for rule ids and plants_table.h documents for plant ids.
 *
 * Follows rules_store.c's idioms closely (house pattern, reviewed hard in
 * M1): same file-IO helpers (path_for/read_whole_file/write_atomic/
 * parse_id_from_json_name lifted near-verbatim), same "one bad file logs
 * and is skipped, never fails boot" tolerance, same id = max-existing + 1
 * rule, same g_rules_mutex-shaped lock around a resident RAM metadata
 * table (g_wrappers here) that more than one task touches -- see
 * wrapper_index.h's Task 7 section for the full cross-task contract
 * (api_v1.c's httpd routes + wrapper_exec.c's decoder-task
 * note_match()/note_error() calls).
 */
#include "wrapper_index.h"
#include "psvm.h"
#include "capability.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "wrappers";

#define WRAPPERS_STORAGE_DIR "/storage/wrappers"
#define WRAPPER_META_MAX     256   /* generous headroom over a 48-char name + 2 numbers + JSON syntax */

/* Native decoders' service-data UUIDs (spec §4's install-time shadowing
 * guard). Duplicated here as plain literals rather than pulled in via a new
 * component dependency: BTHOME_SVC_UUID is public (bthome.h) but MiFlora's
 * (ble_collector.c's private XIAOMI_SVC_UUID) isn't exported by any header
 * at all, so a single, symmetric, clearly-commented local copy of BOTH is
 * the same "small, contained, documented duplication" wrapper_exec.c's own
 * code_uses_aes_ccm() opcode table and bindkey_core.c's NVS_KEY_NAME_MAX_SIZE
 * copy already use in this codebase, rather than a mixed "one real include,
 * one duplicate" that would only save two characters. */
#define WRAPPER_GUARD_BTHOME_SVC_UUID  0xFCD2u
#define WRAPPER_GUARD_MIFLORA_SVC_UUID 0xFE95u

/* Next id to hand out on a future create. Computed at every
 * wrapper_store_load_all() (boot, and every post-install/update/delete
 * reindex) so it always reflects the highest id ever seen on disk, even
 * one skipped for a bad/corrupt meta file -- ids are never reused. */
static uint16_t s_wrapper_next_id = 1;

/* ---------------- Task 7: resident wrapper metadata table --------------
 * See wrapper_index.h's Task 7 section for the full cross-task contract.
 * g_wrappers_mutex guards every field of every entry (RAM only, never held
 * across file I/O) -- same two-phase discipline rules_internal.h's
 * g_rules_mutex documents. Lazily created (wrapper_store_load_all() is
 * called more than once -- boot, then again on every reindex -- unlike
 * rules_store_load_all(), which rules_init() calls exactly once). */
typedef struct {
    bool     in_use;
    uint16_t id;
    char     name[WRAPPER_NAME_MAX + 1];
    bool     enabled;
    uint8_t  match_kind;
    uint32_t match_key;
    uint32_t match_count;
    /* 48, not 64: every producer (wrapper_exec.c's wrapper_err_short()) is a
     * short fixed string ("bytecode exceeds a hub limit", "aes_ccm_decrypt
     * unsupported in this build", ...), all comfortably under 48 bytes --
     * kept tight given g_wrappers's total static cost (WRAPPERS_MAX=16
     * entries) competes with M3 spec §7's already-tight RAM budget. */
    char     last_error[48];
} wrapper_meta_t;

static wrapper_meta_t    g_wrappers[WRAPPERS_MAX];
static SemaphoreHandle_t g_wrappers_mutex;

static void seterr(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen) snprintf(errbuf, errlen, "%s", msg);
}

static bool path_for(uint32_t id, const char *ext, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "%s/%u.%s", WRAPPERS_STORAGE_DIR, (unsigned)id, ext);
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

/* Reads the whole file into buf (capacity buflen); rejects (returns false) a
 * file that turns out to be longer than buflen rather than silently
 * truncating it -- same "confirm the file isn't LONGER than expected"
 * pattern as rules_store.c's/plants.c's read_whole_file().
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

static bool save_meta(const wrapper_meta_t *w)
{
    char path[160];
    if (!path_for(w->id, "json", path, sizeof(path))) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    bool built = cJSON_AddStringToObject(root, "name", w->name) != NULL &&
                cJSON_AddBoolToObject(root, "enabled", w->enabled) != NULL &&
                cJSON_AddNumberToObject(root, "match_kind", w->match_kind) != NULL &&
                cJSON_AddNumberToObject(root, "match_key", w->match_key) != NULL;
    if (!built) { cJSON_Delete(root); return false; }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;
    bool ok = write_atomic(path, txt, strlen(txt));
    cJSON_free(txt);
    return ok;
}

/* Skips whitespace and '#'-to-end-of-line comments (lexer.js's own comment
 * syntax) -- returns false iff this runs off the end of the string. */
static bool skip_ws(const char **pp)
{
    const char *p = *pp;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#') {
            while (*p != '\0' && *p != '\n') p++;
            continue;
        }
        break;
    }
    *pp = p;
    return *p != '\0';
}

/* Matches literal `word` at *pp as a whole word (not a prefix of a longer
 * identifier) and advances *pp past it on success. */
static bool expect_word(const char **pp, const char *word)
{
    const char *p = *pp;
    size_t wlen = strlen(word);
    if (strncmp(p, word, wlen) != 0) return false;
    char after = p[wlen];
    bool boundary = !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
                      (after >= '0' && after <= '9') || after == '_');
    if (!boundary) return false;
    *pp = p + wlen;
    return true;
}

/* Parses the MANDATORY `wrapper "<name>" match <kind> <key>` header out of
 * `source` -- see wrapper_index.h's wrapper_store_upsert() doc comment for
 * why this text, not the compiled bytecode, is the only place this
 * information exists. Deliberately a small hand-rolled scanner, not a full
 * tokenizer: the real grammar (webui/src/lib/psc/parser.js's
 * parseWrapperFile()) is the actual source of truth for whether `source`
 * compiles at all -- this only needs to agree with it for well-formed
 * input, same "convenience, not authority" spirit as api_v1.c's
 * name_from_source() for rules. Numbers accept a `0x` hex prefix or plain
 * decimal, matching lexer.js's own two NUMBER forms; key ranges
 * (0xFFFF for service/manufacturer, 0xFFFFFF for mac_prefix) mirror
 * parser.js's MATCH_KEY_MAX table. Returns false (errbuf set) on anything
 * that doesn't parse. */
static bool wrapper_header_parse(const char *source, uint8_t *kind_out, uint32_t *key_out,
                                 char *errbuf, size_t errlen)
{
    const char *p = source;
    if (!skip_ws(&p) || !expect_word(&p, "wrapper")) {
        seterr(errbuf, errlen, "source must start with wrapper \"<name>\" match <kind> <key>");
        return false;
    }
    if (!skip_ws(&p) || *p != '"') {
        seterr(errbuf, errlen, "expected a quoted wrapper name after 'wrapper'");
        return false;
    }
    p++;
    while (*p != '"' && *p != '\0') {
        if (*p == '\\' && p[1] != '\0') p++;
        p++;
    }
    if (*p != '"') {
        seterr(errbuf, errlen, "unterminated wrapper name string");
        return false;
    }
    p++;

    if (!skip_ws(&p) || !expect_word(&p, "match")) {
        seterr(errbuf, errlen, "expected 'match' after the wrapper name");
        return false;
    }
    if (!skip_ws(&p)) {
        seterr(errbuf, errlen, "expected a match kind (service, manufacturer or mac_prefix)");
        return false;
    }
    uint8_t kind;
    if (expect_word(&p, "service")) kind = WMATCH_SERVICE;
    else if (expect_word(&p, "manufacturer")) kind = WMATCH_MANUFACTURER;
    else if (expect_word(&p, "mac_prefix")) kind = WMATCH_MAC_PREFIX;
    else {
        seterr(errbuf, errlen, "unknown match kind (expected service, manufacturer or mac_prefix)");
        return false;
    }

    if (!skip_ws(&p)) {
        seterr(errbuf, errlen, "expected a match key after the match kind");
        return false;
    }
    char *endp = NULL;
    unsigned long val = strtoul(p, &endp, 0);
    if (endp == p) {
        seterr(errbuf, errlen, "expected a non-negative integer match key");
        return false;
    }
    uint32_t max_key = (kind == WMATCH_MAC_PREFIX) ? 0xFFFFFFu : 0xFFFFu;
    if (val > max_key) {
        seterr(errbuf, errlen, "match key out of range for this match kind");
        return false;
    }
    *kind_out = kind;
    *key_out = (uint32_t)val;
    return true;
}

/* Loads one wrapper's meta JSON: always mirrors it into the g_wrappers RAM
 * table (enabled or not -- Task 7's list/get endpoints must show a disabled
 * wrapper too), and additionally indexes it into *ix ONLY when enabled and
 * its match key indexes cleanly, same as before this task.
 *
 * Updates an EXISTING g_wrappers slot for this id IN PLACE when one is
 * already tracked (rather than wiping the whole table and rebuilding it,
 * which wrapper_store_load_all() does on every single install/update/
 * delete's reindex, not just at boot) -- this is what lets match_count/
 * last_error survive a reindex triggered by installing/deleting some OTHER
 * wrapper (see wrapper_index.h's wrapper_store_note_match()/note_error()
 * doc comments: those diagnostics must not reset to zero every time any
 * wrapper changes). A slot reused for a DIFFERENT id (this id is new, or
 * took over a slot a deleted wrapper vacated) starts both counters fresh,
 * same as a brand new entry. Missing/corrupt meta, or wrapper_index_add()
 * rejecting the key (table full or a colliding (kind,key) pair -- both
 * already logged by the caller below), skip indexing only, same tolerance
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
    cJSON *jname    = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *jenabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    cJSON *jkind    = cJSON_GetObjectItemCaseSensitive(root, "match_kind");
    cJSON *jkey     = cJSON_GetObjectItemCaseSensitive(root, "match_key");
    if (!cJSON_IsString(jname) || !cJSON_IsBool(jenabled) || !cJSON_IsNumber(jkind) || !cJSON_IsNumber(jkey)) {
        ESP_LOGW(TAG, "wrapper %u: meta file missing/invalid fields; skipping", (unsigned)id);
        cJSON_Delete(root);
        return;
    }
    char name[WRAPPER_NAME_MAX + 1];
    strlcpy(name, cJSON_GetStringValue(jname), sizeof(name));
    bool enabled = cJSON_IsTrue(jenabled);
    double kind_d = jkind->valuedouble;
    double key_d = jkey->valuedouble;
    cJSON_Delete(root);

    if (kind_d < WMATCH_SERVICE || kind_d > WMATCH_MAC_PREFIX) {
        ESP_LOGW(TAG, "wrapper %u: bad match_kind %d in meta; skipping", (unsigned)id, (int)kind_d);
        return;
    }
    uint8_t kind = (uint8_t)kind_d;
    uint32_t key = (uint32_t)key_d;

    if (g_wrappers_mutex) {
        xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
        int slot = -1;
        for (int i = 0; i < WRAPPERS_MAX; i++) {
            if (g_wrappers[i].in_use && g_wrappers[i].id == (uint16_t)id) { slot = i; break; }
        }
        bool reuse = (slot >= 0);
        if (slot < 0) {
            for (int i = 0; i < WRAPPERS_MAX; i++) {
                if (!g_wrappers[i].in_use) { slot = i; break; }
            }
        }
        if (slot >= 0) {
            wrapper_meta_t *w = &g_wrappers[slot];
            if (!reuse) {
                memset(w, 0, sizeof(*w));
                w->in_use = true;
                w->id = (uint16_t)id;
            }
            strlcpy(w->name, name, sizeof(w->name));
            w->enabled = enabled;
            w->match_kind = kind;
            w->match_key = key;
        } else {
            ESP_LOGW(TAG, "wrapper %u: RAM metadata table full (WRAPPERS_MAX=%d); not listed", (unsigned)id, WRAPPERS_MAX);
        }
        xSemaphoreGive(g_wrappers_mutex);
    }

    if (!enabled) {
        ESP_LOGI(TAG, "wrapper %u: disabled; not indexed", (unsigned)id);
        return;
    }
    if (wrapper_index_add(ix, kind, key, (uint16_t)id) != 0) {
        ESP_LOGW(TAG, "wrapper %u: not indexed (table full or a duplicate match key); skipping", (unsigned)id);
    }
}

void wrapper_store_load_all(wrapper_index_t *ix)
{
    if (!g_wrappers_mutex) {
        g_wrappers_mutex = xSemaphoreCreateMutex();
        if (!g_wrappers_mutex) {
            ESP_LOGE(TAG, "failed to create wrapper store mutex; wrapper metadata table disabled");
        }
    }

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
    /* Every id actually found on disk this scan -- used below to purge any
     * g_wrappers entry for an id that no longer has a meta file (deleted
     * since the last load), which load_one()'s in-place update alone would
     * otherwise leave stale (it only ever touches ids it SEES). A stack
     * array, not static: WRAPPERS_MAX (16) * 2 B is negligible, unlike the
     * far larger wrapper_meta_t table itself (kept static/file-scope). */
    uint16_t seen_ids[WRAPPERS_MAX];
    size_t seen_n = 0;
    uint32_t max_id = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint32_t id;
        if (!parse_id_from_json_name(de->d_name, &id)) continue;
        if (id > max_id) max_id = id;   /* id stays retired even if indexing below skips it -- never reused */
        load_one(id, ix);
        if (seen_n < WRAPPERS_MAX) seen_ids[seen_n++] = (uint16_t)id;
    }
    closedir(d);
    s_wrapper_next_id = (uint16_t)(max_id + 1);

    if (g_wrappers_mutex) {
        xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
        for (int i = 0; i < WRAPPERS_MAX; i++) {
            if (!g_wrappers[i].in_use) continue;
            bool found = false;
            for (size_t j = 0; j < seen_n; j++) {
                if (seen_ids[j] == g_wrappers[i].id) { found = true; break; }
            }
            if (!found) memset(&g_wrappers[i], 0, sizeof(g_wrappers[i]));
        }
        xSemaphoreGive(g_wrappers_mutex);
    }

    ESP_LOGI(TAG, "wrapper_store_load_all: %u wrapper(s) indexed, next_id=%u",
             (unsigned)ix->count, (unsigned)s_wrapper_next_id);
}

bool wrapper_store_read_psbc(uint16_t id, uint8_t *buf, size_t cap, size_t *len_out)
{
    char path[160];
    if (!path_for(id, "wbc", path, sizeof(path))) return false;
    return read_whole_file(path, buf, cap, len_out);
}

size_t wrapper_store_list(wrapper_info_t *out, size_t max)
{
    if (!out || max == 0 || !g_wrappers_mutex) return 0;
    size_t n = 0;
    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    for (int i = 0; i < WRAPPERS_MAX && n < max; i++) {
        if (!g_wrappers[i].in_use) continue;
        const wrapper_meta_t *w = &g_wrappers[i];
        wrapper_info_t *o = &out[n++];
        o->id = w->id;
        strlcpy(o->name, w->name, sizeof(o->name));
        o->enabled = w->enabled;
        o->match_kind = w->match_kind;
        o->match_key = w->match_key;
        o->match_count = w->match_count;
        strlcpy(o->last_error, w->last_error, sizeof(o->last_error));
    }
    xSemaphoreGive(g_wrappers_mutex);
    return n;
}

bool wrapper_store_get_source(uint16_t id, char *buf, size_t buflen)
{
    if (!buf || buflen == 0 || !g_wrappers_mutex) return false;

    bool known = false;
    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        if (g_wrappers[i].in_use && g_wrappers[i].id == id) { known = true; break; }
    }
    xSemaphoreGive(g_wrappers_mutex);
    if (!known) return false;

    char path[160];
    if (!path_for(id, "wsrc", path, sizeof(path))) return false;
    size_t len;
    if (!read_whole_file(path, buf, buflen - 1, &len)) return false;
    buf[len] = '\0';
    return true;
}

int wrapper_store_upsert(uint16_t *id_inout, const char *name, const char *source,
                         const uint8_t *psbc, size_t psbc_len, bool enabled,
                         char *errbuf, size_t errlen)
{
    if (errbuf && errlen) errbuf[0] = '\0';
    if (!id_inout || !name || !source || !psbc) {
        seterr(errbuf, errlen, "missing argument");
        return ESP_ERR_INVALID_ARG;
    }

    /* Meta/size limits, checked before touching the store or the VM at all
     * -- same ordering rules_upsert() uses. */
    size_t namelen = strlen(name);
    if (namelen == 0 || namelen > WRAPPER_NAME_MAX || strchr(name, '"') != NULL) {
        seterr(errbuf, errlen, "name must be 1-48 characters and must not contain '\"'");
        return ESP_ERR_INVALID_ARG;
    }
    size_t srclen = strlen(source);
    if (srclen == 0 || srclen > WRAPPER_SRC_MAX) {
        seterr(errbuf, errlen, "source must be 1-4096 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    if (psbc_len == 0 || psbc_len > WRAPPER_PSBC_MAX) {
        seterr(errbuf, errlen, "bytecode must be 1-2048 bytes");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t match_kind = 0;
    uint32_t match_key = 0;
    if (!wrapper_header_parse(source, &match_kind, &match_key, errbuf, errlen)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Spec §4's shadowing guard: a wrapper claiming BTHome's or MiFlora's
     * own service UUID would silently shadow that native decode path,
     * since a wrapper match returns before either native check ever runs
     * (ble_collector.c's decode_adv_item(): BTHome dispatch happens first,
     * then the wrapper index, then native MiFlora last). */
    if (match_kind == WMATCH_SERVICE &&
        (match_key == WRAPPER_GUARD_BTHOME_SVC_UUID || match_key == WRAPPER_GUARD_MIFLORA_SVC_UUID)) {
        seterr(errbuf, errlen, "match key collides with a built-in decoder (BTHome 0xFCD2 or MiFlora 0xFE95)");
        return ESP_ERR_INVALID_ARG;
    }

    /* Bytecode itself: header/limits/refs bounds (spec §2's forward-
     * compatibility gate) -- reject unknown capability ids / builtins here,
     * at upload, not at first run. Wrapper dialect never uses CALL_BUILTIN
     * (psvm.h), so builtins_impl=0. */
    psvm_prog_t prog;
    psvm_err_t verr = psvm_validate(psbc, psbc_len, PSVM_DIALECT_WRAPPERS,
                                    CAPABILITY_COUNT - 1, 0, &prog);
    if (verr != PSVM_OK) {
        seterr(errbuf, errlen, psvm_err_str(verr));
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_wrappers_mutex) {
        g_wrappers_mutex = xSemaphoreCreateMutex();
        if (!g_wrappers_mutex) {
            seterr(errbuf, errlen, "wrapper store not initialised");
            return ESP_ERR_INVALID_ARG;
        }
    }

    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    bool is_new = (*id_inout == 0);
    int idx = -1;
    uint16_t id;
    if (is_new) {
        for (int i = 0; i < WRAPPERS_MAX; i++) {
            if (!g_wrappers[i].in_use) { idx = i; break; }
        }
        if (idx < 0) {
            xSemaphoreGive(g_wrappers_mutex);
            seterr(errbuf, errlen, "wrapper table full (16 wrappers max)");
            return ESP_ERR_NO_MEM;
        }
        id = s_wrapper_next_id++;
    } else {
        for (int i = 0; i < WRAPPERS_MAX; i++) {
            if (g_wrappers[i].in_use && g_wrappers[i].id == *id_inout) { idx = i; break; }
        }
        if (idx < 0) {
            xSemaphoreGive(g_wrappers_mutex);
            seterr(errbuf, errlen, "unknown wrapper id");
            return ESP_ERR_INVALID_ARG;
        }
        id = *id_inout;
    }

    /* Duplicate-(kind,key) check against every OTHER currently-tracked
     * wrapper, enabled or not (a disabled duplicate would otherwise
     * silently fail to index the moment someone enables it, instead of
     * being caught here where the user can see it). Reuses
     * wrapper_index_add()'s own dedup logic on a throwaway index rather
     * than a second hand-written duplicate scan -- exactly the "surface
     * wrapper_index_add()'s -1 as a 400, not a 500" ask. */
    wrapper_index_t tmp;
    wrapper_index_init(&tmp);
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        if (!g_wrappers[i].in_use || i == idx) continue;
        wrapper_index_add(&tmp, g_wrappers[i].match_kind, g_wrappers[i].match_key, g_wrappers[i].id);
    }
    if (wrapper_index_add(&tmp, match_kind, match_key, id) != 0) {
        xSemaphoreGive(g_wrappers_mutex);
        seterr(errbuf, errlen, "match key already used by another wrapper");
        return ESP_ERR_INVALID_ARG;
    }

    wrapper_meta_t *w = &g_wrappers[idx];
    if (is_new) {
        memset(w, 0, sizeof(*w));
        w->in_use = true;
        w->id = id;
    }
    strlcpy(w->name, name, sizeof(w->name));
    w->enabled = enabled;
    w->match_kind = match_kind;
    w->match_key = match_key;
    wrapper_meta_t snapshot = *w;
    xSemaphoreGive(g_wrappers_mutex);

    char path[160];
    bool ok = path_for(id, "wsrc", path, sizeof(path)) && write_atomic(path, source, srclen);
    if (ok) ok = path_for(id, "wbc", path, sizeof(path)) && write_atomic(path, psbc, psbc_len);
    if (ok) ok = save_meta(&snapshot);

    *id_inout = id;
    if (!ok) {
        /* RAM already reflects the change and will not revert (same "RAM
         * can run ahead of a failed write" contract as rules_upsert()'s
         * own comment) -- id/slot stay claimed; a retry with the same id
         * re-attempts the writes. */
        seterr(errbuf, errlen, "failed to persist wrapper to storage");
        ESP_LOGE(TAG, "wrapper %u: failed to persist to %s", (unsigned)id, WRAPPERS_STORAGE_DIR);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wrapper %u (\"%s\") %s", (unsigned)id, snapshot.name, is_new ? "created" : "updated");
    return ESP_OK;
}

bool wrapper_store_delete(uint16_t id)
{
    if (!g_wrappers_mutex) return false;

    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        if (g_wrappers[i].in_use && g_wrappers[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(g_wrappers_mutex);
        return false;
    }
    memset(&g_wrappers[idx], 0, sizeof(g_wrappers[idx]));
    xSemaphoreGive(g_wrappers_mutex);

    char path[160];
    if (path_for(id, "wsrc", path, sizeof(path))) remove(path);
    if (path_for(id, "wbc", path, sizeof(path))) remove(path);
    if (path_for(id, "json", path, sizeof(path))) remove(path);
    ESP_LOGI(TAG, "wrapper %u deleted", (unsigned)id);
    return true;
}

void wrapper_store_note_match(uint16_t id)
{
    if (!g_wrappers_mutex) return;
    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        if (g_wrappers[i].in_use && g_wrappers[i].id == id) {
            g_wrappers[i].match_count++;
            break;
        }
    }
    xSemaphoreGive(g_wrappers_mutex);
}

void wrapper_store_note_error(uint16_t id, const char *msg)
{
    if (!g_wrappers_mutex) return;
    xSemaphoreTake(g_wrappers_mutex, portMAX_DELAY);
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        if (g_wrappers[i].in_use && g_wrappers[i].id == id) {
            if (msg && msg[0] != '\0') strlcpy(g_wrappers[i].last_error, msg, sizeof(g_wrappers[i].last_error));
            else g_wrappers[i].last_error[0] = '\0';
            break;
        }
    }
    xSemaphoreGive(g_wrappers_mutex);
}
