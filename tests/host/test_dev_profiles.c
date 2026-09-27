/* Host test for dev_profiles.c + dev_profiles_builtin.c (pure core, no cJSON). */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "dev_profiles.h"
#include "capability.h"

static dev_profile_t mk_profile(const char *manuf, const char *model, uint8_t cap_id) {
    dev_profile_t p; memset(&p, 0, sizeof p);
    snprintf(p.manufacturer, sizeof p.manufacturer, "%s", manuf);
    snprintf(p.model, sizeof p.model, "%s", model);
    p.entry_count = 1;
    p.entries[0].kind = DEV_PROFILE_KIND_DP;
    p.entries[0].dp_id = 3; p.entries[0].cap_id = cap_id;
    p.entries[0].scale = 1.0f; p.entries[0].offset = 0.0f;
    return p;
}

int main(void) {
    /* --- built-in seed contains the ZS-301Z profile --- */
    const dev_profile_t *zs = dev_profile_match("_TZE284_o9ofysmo", "TS0601", NULL);
    assert(zs != NULL);
    bool saw_soil = false, saw_suppress = false, saw_temp_scaled = false;
    for (int i = 0; i < zs->entry_count; i++) {
        const dev_profile_entry_t *e = &zs->entries[i];
        if (e->kind == DEV_PROFILE_KIND_DP && e->dp_id == 3 && e->cap_id == CAP_SOIL_MOISTURE) saw_soil = true;
        if (e->kind == DEV_PROFILE_KIND_DP && e->dp_id == 5 && e->cap_id == CAP_AIR_TEMPERATURE
            && e->scale > 0.09f && e->scale < 0.11f) saw_temp_scaled = true;
        if (e->kind == DEV_PROFILE_KIND_SUPPRESS && e->source_cluster == 0x0405) saw_suppress = true;
    }
    assert(saw_soil && saw_suppress && saw_temp_scaled);
    assert(dev_profile_validate(zs));

    /* --- no match returns NULL --- */
    assert(dev_profile_match("nope", "nope", NULL) == NULL);

    /* --- user store precedence over built-in --- */
    dev_profile_store_t user; dev_profile_store_init(&user);
    dev_profile_t override = mk_profile("_TZE284_o9ofysmo", "TS0601", CAP_BATTERY_LEVEL);
    assert(dev_profile_store_upsert(&user, &override) == 0);
    const dev_profile_t *m = dev_profile_match("_TZE284_o9ofysmo", "TS0601", &user);
    assert(m != NULL && m->entries[0].cap_id == CAP_BATTERY_LEVEL);   /* user wins */

    /* --- upsert replaces in place on the same match key --- */
    dev_profile_t override2 = mk_profile("_TZE284_o9ofysmo", "TS0601", CAP_SOIL_MOISTURE);
    assert(dev_profile_store_upsert(&user, &override2) == 0);
    assert(user.count == 1);

    /* --- validate rejects an out-of-range cap and the reserved cluster kind --- */
    dev_profile_t bad = mk_profile("x", "y", CAPABILITY_COUNT);
    assert(!dev_profile_validate(&bad));
    dev_profile_t reserved = mk_profile("x", "y", CAP_SOIL_MOISTURE);
    reserved.entries[0].kind = DEV_PROFILE_KIND_CLUSTER;
    assert(!dev_profile_validate(&reserved));

    /* --- suppress entry needs a nonzero source_cluster --- */
    dev_profile_t sup = mk_profile("x", "y", CAP_SOIL_MOISTURE);
    sup.entries[0].kind = DEV_PROFILE_KIND_SUPPRESS; sup.entries[0].source_cluster = 0;
    assert(!dev_profile_validate(&sup));
    sup.entries[0].source_cluster = 0x0405;
    assert(dev_profile_validate(&sup));

    /* --- cap name resolution --- */
    uint8_t id = 0xFF;
    assert(dev_profile_resolve_cap("soil.moisture", &id) && id == CAP_SOIL_MOISTURE);
    assert(!dev_profile_resolve_cap("not.a.cap", &id));

    printf("dev_profiles ok\n");
    return 0;
}
