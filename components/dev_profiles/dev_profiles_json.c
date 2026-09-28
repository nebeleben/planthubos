/* cJSON parse/emit for a dev_profile_t and its LittleFS persistence. Kept
 * OUT of dev_profiles.c so the pure core stays host-testable without cJSON.
 * Schema (spec section 3): { "match": {"manufacturer","model"}, "label"?,
 * "entries": [ {"kind":"dp","dp_id","cap","scale","offset"} |
 *              {"kind":"suppress","source_cluster"} ] }. Cap targets are
 * NAMES, resolved via dev_profile_resolve_cap; source_cluster accepts a JSON
 * number or a "0x0405" string. */
#include "dev_profiles.h"
#include "capability.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_PROFILE_PATH     "/storage/dev_profiles.json"
#define DEV_PROFILE_TMP_PATH "/storage/dev_profiles.tmp"

static uint16_t parse_cluster(const cJSON *v) {
    if (cJSON_IsNumber(v)) return (uint16_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return (uint16_t)strtol(v->valuestring, NULL, 0);
    return 0;
}

bool dev_profile_from_json(const char *json, dev_profile_t *out) {
    if (!json || !out) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    memset(out, 0, sizeof *out);
    bool ok = true;
    const cJSON *match = cJSON_GetObjectItemCaseSensitive(root, "match");
    const cJSON *mf = match ? cJSON_GetObjectItemCaseSensitive(match, "manufacturer") : NULL;
    const cJSON *md = match ? cJSON_GetObjectItemCaseSensitive(match, "model") : NULL;
    if (!cJSON_IsString(mf) || !cJSON_IsString(md)) { ok = false; goto done; }
    snprintf(out->manufacturer, sizeof out->manufacturer, "%s", mf->valuestring);
    snprintf(out->model, sizeof out->model, "%s", md->valuestring);
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(root, "label");
    if (cJSON_IsString(label)) snprintf(out->label, sizeof out->label, "%s", label->valuestring);
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(entries)) { ok = false; goto done; }
    const cJSON *e;
    cJSON_ArrayForEach(e, entries) {
        if (out->entry_count >= DEV_PROFILE_MAX_ENTRIES) { ok = false; break; }
        dev_profile_entry_t *slot = &out->entries[out->entry_count];
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(e, "kind");
        if (!cJSON_IsString(kind)) { ok = false; break; }
        if (strcmp(kind->valuestring, "dp") == 0) {
            const cJSON *dp = cJSON_GetObjectItemCaseSensitive(e, "dp_id");
            const cJSON *cap = cJSON_GetObjectItemCaseSensitive(e, "cap");
            const cJSON *scale = cJSON_GetObjectItemCaseSensitive(e, "scale");
            const cJSON *offset = cJSON_GetObjectItemCaseSensitive(e, "offset");
            if (!cJSON_IsNumber(dp) || !cJSON_IsString(cap)) { ok = false; break; }
            if (!dev_profile_resolve_cap(cap->valuestring, &slot->cap_id)) { ok = false; break; }
            slot->kind = DEV_PROFILE_KIND_DP;
            slot->dp_id = (uint8_t)dp->valuedouble;
            slot->scale = cJSON_IsNumber(scale) ? (float)scale->valuedouble : 1.0f;
            slot->offset = cJSON_IsNumber(offset) ? (float)offset->valuedouble : 0.0f;
        } else if (strcmp(kind->valuestring, "suppress") == 0) {
            slot->kind = DEV_PROFILE_KIND_SUPPRESS;
            slot->source_cluster = parse_cluster(cJSON_GetObjectItemCaseSensitive(e, "source_cluster"));
        } else { ok = false; break; }
        out->entry_count++;
    }
done:
    cJSON_Delete(root);
    return ok && dev_profile_validate(out);
}

char *dev_profile_to_json(const dev_profile_t *p) {
    if (!p) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *match = cJSON_AddObjectToObject(root, "match");
    cJSON_AddStringToObject(match, "manufacturer", p->manufacturer);
    cJSON_AddStringToObject(match, "model", p->model);
    if (p->label[0]) cJSON_AddStringToObject(root, "label", p->label);
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < p->entry_count; i++) {
        const dev_profile_entry_t *e = &p->entries[i];
        cJSON *je = cJSON_CreateObject();
        if (e->kind == DEV_PROFILE_KIND_DP) {
            const capability_t *c = capability_get(e->cap_id);
            cJSON_AddStringToObject(je, "kind", "dp");
            cJSON_AddNumberToObject(je, "dp_id", e->dp_id);
            cJSON_AddStringToObject(je, "cap", c ? c->name : "");
            cJSON_AddNumberToObject(je, "scale", e->scale);
            cJSON_AddNumberToObject(je, "offset", e->offset);
        } else {
            char hex[8]; snprintf(hex, sizeof hex, "0x%04X", e->source_cluster);
            cJSON_AddStringToObject(je, "kind", "suppress");
            cJSON_AddStringToObject(je, "source_cluster", hex);
        }
        cJSON_AddItemToArray(entries, je);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

void dev_profile_user_load(dev_profile_store_t *s) {
    dev_profile_store_init(s);
    FILE *f = fopen(DEV_PROFILE_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)n, f); fclose(f); buf[rd] = '\0';
    cJSON *arr = cJSON_Parse(buf);
    if (cJSON_IsArray(arr)) {
        const cJSON *pe;
        cJSON_ArrayForEach(pe, arr) {
            char *one = cJSON_PrintUnformatted(pe);
            dev_profile_t prof;
            if (one && dev_profile_from_json(one, &prof)) dev_profile_store_upsert(s, &prof);
            free(one);
        }
    }
    cJSON_Delete(arr);
    free(buf);
}

bool dev_profile_user_save(const dev_profile_store_t *s) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s->count; i++) {
        char *one = dev_profile_to_json(&s->p[i]);
        if (one) { cJSON_AddItemToArray(arr, cJSON_Parse(one)); free(one); }
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!out) return false;
    FILE *f = fopen(DEV_PROFILE_TMP_PATH, "wb");
    if (!f) { free(out); return false; }
    size_t len = strlen(out);
    bool ok = fwrite(out, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    free(out);
    if (!ok) { remove(DEV_PROFILE_TMP_PATH); return false; }
    if (rename(DEV_PROFILE_TMP_PATH, DEV_PROFILE_PATH) != 0) { remove(DEV_PROFILE_TMP_PATH); return false; }
    return true;
}
