/* rules_resolver.c -- turns a PSBC ref (plant("X").cap / device("id").cap)
 * into a resolved f32 + age_s + ready, by scanning the plants table and the
 * live device registry. Runs at every evaluation (spec: "cheap table scans,
 * <=32 sensors") -- no caching, so a rename/rebind is visible on the very
 * next evaluation with no rule reload.
 *
 * Task 6 (M2 spec Sec.7): rules_resolve()'s signature is UNCHANGED from M1
 * -- only these internals move off the V1 MiFlora-shaped registry shim
 * (registry_compat.h) onto the real V2 registry (registry.h/capability.h):
 * plant refs now go through plants_cap_value() (plants.h), device refs
 * through registry_find() + the capability slot directly. M1's stored
 * bytecode and saved rules reference capability ids 0-4 numerically
 * (RULES_CAP_MAX_ID, rules_internal.h) -- those are capability.h's frozen
 * ids (soil.moisture..battery.level), so an unmodified M1 rule resolves
 * identically through this new path. */
#include "rules_internal.h"
#include "plants.h"
#include "plants_table.h"
#include "data_core.h"
#include "registry.h"
#include "capability.h"
#include "app_config.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

const char *rules_cap_name(uint8_t capability_id)
{
    const capability_t *c = capability_get(capability_id);
    return c ? c->name : "?";
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

/* Shared registry_t snapshot scratch (M1/M2 fixwave): resolve_plant() and
 * resolve_device() each used to declare their OWN `static registry_t reg`
 * (~1984 B apiece, ~3968 B total in .bss). rules_resolve() below calls
 * exactly one of the two per invocation (the r.kind == 0 ? plant : device
 * branch) -- they are never simultaneously live -- and both already
 * documented themselves as touched only from the caller's single
 * evaluation-serialized task, so nothing about sharing one file-scope
 * static changes that safety argument, only halves the static footprint. */
static registry_t s_reg_snap;

/* plant("<name>") kind: name -> plants table entry -> that capability's
 * binding -> registry value, via plants_cap_value() (plants.h), which does
 * the binding lookup + registry_find() + slot read in one call over a
 * registry snapshot this function already took. Failure reasons (spec Sec.7
 * / task-6 brief step 3): "no plant X" (unknown name), "plant X has no
 * <cap> bound" (name known, but this ref's capability was never bound on
 * it -- was "plant X has no probe" pre-M2), "device never heard" (bound,
 * but plants_cap_value() still can't produce a value -- the bound device
 * isn't in this snapshot, or its slot has never been written; was "probe
 * never heard" pre-M2). */
static bool resolve_plant(const char *name, uint8_t capability, uint8_t field,
                          uint32_t now_uptime_s, psvm_ref_val_t *out, char *why, size_t whylen)
{
    /* plants_cap_value() takes its own wall-clock read for age_s (see its
     * doc comment in plants.h) -- this function has no use for the shared
     * now_uptime_s resolve_device() below needs for its own registry-slot
     * math, but keeps the parameter so both resolver kinds share one call
     * shape off rules_resolve(). */
    (void)now_uptime_s;
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

    if (capability >= CAPABILITY_COUNT || !snap.p[idx].cap_bound[capability]) {
        ref_not_ready(out);
        if (why && whylen) {
            snprintf(why, whylen, "plant \"%s\" has no %s bound", name, rules_cap_name(capability));
        }
        return false;
    }
    uint8_t plant_id = snap.p[idx].id;

    data_core_snapshot(&s_reg_snap);

    float value; uint32_t age_s;
    if (!plants_cap_value(plant_id, capability, &s_reg_snap, &value, &age_s)) {
        ref_not_ready(out);
        if (why && whylen) snprintf(why, whylen, "device never heard");
        return false;
    }

    out->ready = true;
    out->age_s = age_s;
    out->value = (field == 1) ? (float)age_s : value;
    return true;
}

/* device("<id>") kind: id is either a sensor display name (app_config's
 * mac-keyed name store, checked against every live BLE device in the
 * snapshot -- the only kind that store can name) or a device-id string
 * device_id_parse() accepts: the canonical "kind:HEX" form (capability.h
 * Sec.2), or M1's legacy bare "AA:BB:CC:DD:EE:FF" colon-mac literal, which
 * device_id_parse() itself maps onto DEV_KIND_BLE -- exactly what an
 * unmodified M1 device() ref (always BLE, the only kind that existed then)
 * needs to keep resolving unchanged. */
static bool resolve_device(const char *id, uint8_t capability, uint8_t field,
                           uint32_t now_uptime_s, psvm_ref_val_t *out, char *why, size_t whylen)
{
    data_core_snapshot(&s_reg_snap);

    device_id_t dev = {0};
    bool have_dev = false;
    char nm[33];
    for (int i = 0; i < REGISTRY_MAX_DEVICES && !have_dev; i++) {
        const device_entry_t *e = &s_reg_snap.devices[i];
        if (!e->in_use || e->id.kind != DEV_KIND_BLE) continue;
        if (app_config_get_sensor_name(e->id.addr, nm) && strcmp(nm, id) == 0) {
            dev = e->id;
            have_dev = true;
        }
    }
    if (!have_dev) have_dev = device_id_parse(id, &dev);

    int ridx = have_dev ? registry_find(&s_reg_snap, &dev) : -1;
    if (ridx < 0 || capability >= CAPABILITY_COUNT) {
        ref_not_ready(out);
        set_why(why, whylen, "device \"%s\" never heard", id);
        return false;
    }

    const cap_slot_t *slot = &s_reg_snap.devices[ridx].caps[capability];
    if (!slot->valid) {
        ref_not_ready(out);
        if (why && whylen) {
            snprintf(why, whylen, "device \"%s\" %s never reported", id, rules_cap_name(capability));
        }
        return false;
    }

    out->ready = true;
    out->age_s = (now_uptime_s >= slot->updated_s) ? (now_uptime_s - slot->updated_s) : 0;
    out->value = (field == 1) ? (float)out->age_s : capability_decode(capability, slot->raw);
    return true;
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
