/* rules_resolver.c -- capability shim (spec §4 "Resolver"): turns a PSBC
 * ref (plant("X").cap / device("id").cap) into a resolved f32 + age_s +
 * ready, by scanning the plants table and the live sensor registry. Runs at
 * every evaluation (spec: "cheap table scans, <=32 sensors") -- no caching,
 * so a rename/rebind is visible on the very next evaluation with no rule
 * reload. This is the M1 shim spec §4 says M2 will swap the internals of;
 * the rules_resolve() signature is the part that survives. */
#include "rules_internal.h"
#include "plants.h"
#include "plants_table.h"
#include "data_core.h"
#include "registry.h"
#include "app_config.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

/* Capability ids, spec §2: 0 soil.moisture, 1 air.temperature,
 * 2 light.illuminance, 3 soil.conductivity, 4 battery.level. */
static const char *const CAP_NAMES[] = {
    "soil.moisture", "air.temperature", "light.illuminance",
    "soil.conductivity", "battery.level",
};
#define CAP_COUNT (sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0]))

const char *rules_cap_name(uint8_t capability_id)
{
    if (capability_id >= CAP_COUNT) return "?";
    return CAP_NAMES[capability_id];
}

static void ref_not_ready(psvm_ref_val_t *out)
{
    out->value = 0.0f;
    out->age_s = 0;
    out->ready = false;
}

static void set_why(char *why, size_t whylen, const char *fmt, const char *arg)
{
    if (!why || whylen == 0) return;
    snprintf(why, whylen, fmt, arg);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* True + fills 6-byte mac iff s is exactly "AA:BB:CC:DD:EE:FF" (upper or
 * lower hex, spec §1 grammar) -- strict: exactly two hex digits per byte,
 * ':' separators in exactly the right five spots, nothing else. */
static bool parse_mac_literal(const char *s, uint8_t mac[6])
{
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[i * 3]);
        int lo = hex_nibble(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':') return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Fills *out/why from one sensor_entry_t's field (spec §2 capability ids,
 * §1 ".age" semantics: never-heard is not-ready, not infinity). kind_word is
 * "plant"/"device", name is that ref's display name, purely for the message. */
static bool resolve_from_entry(const sensor_entry_t *e, uint8_t capability, uint8_t field,
                               uint32_t now_uptime_s, const char *kind_word, const char *name,
                               psvm_ref_val_t *out, char *why, size_t whylen)
{
    const mibeacon_t *m = &e->latest;
    bool has;
    float value = 0.0f;
    switch (capability) {
    case 0: has = m->has_moisture;     value = (float)m->moisture_pct;      break;
    case 1: has = m->has_temp;         value = (float)m->temp_dc / 10.0f;   break;
    case 2: has = m->has_lux;          value = (float)m->lux;               break;
    case 3: has = m->has_conductivity; value = (float)m->conductivity_us;   break;
    case 4: has = m->has_battery;      value = (float)m->battery_pct;       break;
    default: has = false; break;
    }
    if (!has) {
        ref_not_ready(out);
        if (why && whylen) {
            snprintf(why, whylen, "%s \"%s\" %s never reported", kind_word, name, rules_cap_name(capability));
        }
        return false;
    }
    out->ready = true;
    out->age_s = (now_uptime_s >= e->last_seen_s) ? (now_uptime_s - e->last_seen_s) : 0;
    out->value = (field == 1) ? (float)out->age_s : value;
    return true;
}

/* plant("<name>") kind: name -> plants table entry -> bound mac -> registry
 * entry -> field (spec §1 resolver bullet's exact wording for the two
 * failure reasons). */
static bool resolve_plant(const char *name, uint8_t capability, uint8_t field,
                          uint32_t now_uptime_s, psvm_ref_val_t *out, char *why, size_t whylen)
{
    /* static: only ever touched from the caller's task (engine task, or a
     * future rules_test() caller serialized by rules_engine.c's evaluation
     * mutex -- see rules_engine.c) -- same "big local off a small task
     * stack" idiom as sampler.c's sample_once(). */
    static plants_table_t snap;
    plants_snapshot(&snap);

    int idx = -1;
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (snap.p[i].in_use && strcmp(snap.p[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        ref_not_ready(out);
        set_why(why, whylen, "no plant \"%s\"", name);
        return false;
    }
    if (!snap.p[idx].mac_valid) {
        ref_not_ready(out);
        set_why(why, whylen, "plant \"%s\" has no probe", name);
        return false;
    }

    static legacy_registry_t reg;   /* M2-SHIM */
    data_core_snapshot_legacy(&reg);   /* M2-SHIM */
    int ridx = legacy_registry_find(&reg, snap.p[idx].mac);   /* M2-SHIM */
    if (ridx < 0) {
        ref_not_ready(out);
        if (why && whylen) snprintf(why, whylen, "probe never heard");
        return false;
    }
    return resolve_from_entry(&reg.sensors[ridx], capability, field, now_uptime_s,
                              "plant", name, out, why, whylen);
}

/* device("<id>") kind: id is either a sensor display name
 * (app_config_get_sensor_name over every live registry mac) or a literal
 * "AA:BB:CC:DD:EE:FF" mac (spec §1 grammar / brief step 2). */
static bool resolve_device(const char *id, uint8_t capability, uint8_t field,
                           uint32_t now_uptime_s, psvm_ref_val_t *out, char *why, size_t whylen)
{
    static legacy_registry_t reg;   /* M2-SHIM */
    data_core_snapshot_legacy(&reg);   /* M2-SHIM */

    int ridx = -1;
    char nm[33];
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        if (!reg.sensors[i].in_use) continue;
        if (app_config_get_sensor_name(reg.sensors[i].mac, nm) && strcmp(nm, id) == 0) {
            ridx = i;
            break;
        }
    }
    if (ridx < 0) {
        uint8_t mac[6];
        if (parse_mac_literal(id, mac)) {
            ridx = legacy_registry_find(&reg, mac);   /* M2-SHIM */
        }
    }
    if (ridx < 0) {
        ref_not_ready(out);
        set_why(why, whylen, "device \"%s\" never heard", id);
        return false;
    }
    return resolve_from_entry(&reg.sensors[ridx], capability, field, now_uptime_s,
                              "device", id, out, why, whylen);
}

bool rules_resolve(const psvm_prog_t *prog, uint16_t ref_idx, psvm_ref_val_t *out,
                   char *why, size_t whylen)
{
    if (why && whylen) why[0] = '\0';
    if (!out) return false;
    ref_not_ready(out);
    if (!prog || ref_idx >= prog->ref_count) {
        set_why(why, whylen, "%s", "bad ref index");
        return false;
    }

    psvm_ref_t r = psvm_get_ref(prog, ref_idx);
    uint16_t namelen = 0;
    const char *nameptr = psvm_get_str(prog, r.name_const, &namelen);
    char name[64];
    if (!nameptr) {
        name[0] = '\0';
    } else {
        if (namelen >= sizeof(name)) namelen = sizeof(name) - 1;
        memcpy(name, nameptr, namelen);
        name[namelen] = '\0';
    }

    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    if (r.kind == 0) {
        return resolve_plant(name, r.capability, r.field, now_uptime_s, out, why, whylen);
    } else {
        return resolve_device(name, r.capability, r.field, now_uptime_s, out, why, whylen);
    }
}
