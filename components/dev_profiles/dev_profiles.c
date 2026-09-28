#include "dev_profiles.h"
#include "capability.h"
#include <math.h>
#include <string.h>

void dev_profile_store_init(dev_profile_store_t *s) { memset(s, 0, sizeof *s); }

static int store_find(const dev_profile_store_t *s, const char *manuf, const char *model) {
    for (int i = 0; i < s->count; i++)
        if (strncmp(s->p[i].manufacturer, manuf, DEV_PROFILE_STR_MAX) == 0 &&
            strncmp(s->p[i].model, model, DEV_PROFILE_STR_MAX) == 0)
            return i;
    return -1;
}

int dev_profile_store_upsert(dev_profile_store_t *s, const dev_profile_t *p) {
    int idx = store_find(s, p->manufacturer, p->model);
    if (idx >= 0) { s->p[idx] = *p; return idx; }
    if (s->count >= DEV_PROFILE_MAX_USER) return -1;
    idx = s->count;
    s->p[idx] = *p;
    s->count++;
    return idx;
}

bool dev_profile_store_remove(dev_profile_store_t *s, const char *manuf, const char *model) {
    int idx = store_find(s, manuf, model);
    if (idx < 0) return false;
    for (int i = idx; i < s->count - 1; i++) s->p[i] = s->p[i + 1];
    s->count--;
    memset(&s->p[s->count], 0, sizeof s->p[s->count]);
    return true;
}

bool dev_profile_resolve_cap(const char *name, uint8_t *cap_id_out) {
    if (!name) return false;
    const capability_t *c = capability_by_name(name);
    if (!c) return false;
    if (cap_id_out) *cap_id_out = c->id;
    return true;
}

bool dev_profile_validate(const dev_profile_t *p) {
    if (!p) return false;
    if (p->manufacturer[0] == '\0' || p->model[0] == '\0') return false;
    if (p->entry_count > DEV_PROFILE_MAX_ENTRIES) return false;
    for (int i = 0; i < p->entry_count; i++) {
        const dev_profile_entry_t *e = &p->entries[i];
        switch (e->kind) {
        case DEV_PROFILE_KIND_DP:
            if (e->cap_id >= CAPABILITY_COUNT) return false;
            if (!isfinite(e->scale) || !isfinite(e->offset)) return false;
            break;
        case DEV_PROFILE_KIND_SUPPRESS:
            if (e->source_cluster == 0) return false;
            break;
        default:   /* CLUSTER (reserved) and any unknown kind */
            return false;
        }
    }
    return true;
}

const dev_profile_t *dev_profile_match(const char *manuf, const char *model,
                                       const dev_profile_store_t *user) {
    if (!manuf || !model) return NULL;
    if (user) {
        int idx = store_find(user, manuf, model);
        if (idx >= 0) return &user->p[idx];
    }
    for (int i = 0; i < dev_profile_builtin_count(); i++) {
        const dev_profile_t *b = dev_profile_builtin(i);
        if (strncmp(b->manufacturer, manuf, DEV_PROFILE_STR_MAX) == 0 &&
            strncmp(b->model, model, DEV_PROFILE_STR_MAX) == 0)
            return b;
    }
    return NULL;
}
