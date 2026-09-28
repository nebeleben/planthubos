#include "dev_profiles.h"
#include "capability.h"

/* Read-only, embedded in firmware so matching works out of the box. Cap ids
 * are the frozen enum values (capability.h) -- the NAME->id resolution the
 * spec requires happens at authoring time for the built-in seed, and at parse
 * time (dev_profile_resolve_cap) for user/AI profiles. The Arteco ZS-301Z
 * (Tuya _TZE284_o9ofysmo / TS0601) reports soil moisture on the standard
 * Humidity cluster 0x0405, which is why the suppress entry is essential. */
static const dev_profile_t s_builtins[] = {
    {
        .manufacturer = "_TZE284_o9ofysmo",
        .model = "TS0601",
        .label = "Arteco ZS-301Z 4-in-1 soil sensor",
        .entry_count = 5,
        .entries = {
            { .kind = DEV_PROFILE_KIND_DP, .dp_id = 3,   .cap_id = CAP_SOIL_MOISTURE,     .scale = 1.0f, .offset = 0.0f },
            { .kind = DEV_PROFILE_KIND_DP, .dp_id = 101, .cap_id = CAP_AIR_HUMIDITY,      .scale = 1.0f, .offset = 0.0f },
            { .kind = DEV_PROFILE_KIND_DP, .dp_id = 102, .cap_id = CAP_LIGHT_ILLUMINANCE, .scale = 1.0f, .offset = 0.0f },
            { .kind = DEV_PROFILE_KIND_DP, .dp_id = 5,   .cap_id = CAP_AIR_TEMPERATURE,   .scale = 0.1f, .offset = 0.0f },
            { .kind = DEV_PROFILE_KIND_SUPPRESS, .source_cluster = 0x0405 },
        },
    },
};

int dev_profile_builtin_count(void) {
    return (int)(sizeof s_builtins / sizeof s_builtins[0]);
}

const dev_profile_t *dev_profile_builtin(int i) {
    if (i < 0 || i >= dev_profile_builtin_count()) return 0;
    return &s_builtins[i];
}
