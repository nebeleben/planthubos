#include "devices_json.h"
#include "app_config.h"
#include "swarm_store.h"
#include "capability.h"
#include "plants.h"
#include "bthome.h"
#include "data_core.h"
#include "ble_collector.h"
#include "gatt_sched.h"
#include "gatt_engine.h"
#include "actor.h"
#include "action.h"
#include <stdio.h>

/* now_uptime_s - last_seen_s, both esp_timer uptime seconds off the same
 * monotonic clock -- floor-at-zero clamp, same defensive spirit as
 * swarm.c's identical age conversion. */
static uint32_t age_s(uint32_t now_uptime_s, uint32_t last_seen_s)
{
    return (now_uptime_s >= last_seen_s) ? (now_uptime_s - last_seen_s) : 0;
}

static const char *device_kind_str(uint8_t kind)
{
    switch ((device_kind_t)kind) {
    case DEV_KIND_BLE:    return "ble";
    case DEV_KIND_ESPNOW: return "espnow";
    case DEV_KIND_ZIGBEE: return "zb";
    }
    return "?";
}

static void via_node_mac_str(char *buf, size_t buflen, const uint8_t mac[6])
{
    snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* action.h's action_param_t, rendered the same way device_kind_str() above
 * renders device_kind_t -- a string the UI can switch on directly rather
 * than re-deriving meaning from a bare 0/1. */
static const char *action_param_str(action_param_t p)
{
    switch (p) {
    case ACTION_PARAM_NONE:       return "none";
    case ACTION_PARAM_DURATION_S: return "duration_s";
    }
    return "none";
}

/* actor_table.h's actor_verdict_t, as it can appear in
 * actor_pair_state_t.live_verdict -- see that struct's own doc comment for
 * why only these three values are reachable here (BOUND and LOCKOUT
 * cannot: this is always evaluated as if for a MANUAL request). */
static const char *live_verdict_str(actor_verdict_t v)
{
    switch (v) {
    case ACTOR_OK:               return "ok";
    case ACTOR_REFUSED_UNKNOWN:  return "unknown";
    case ACTOR_REFUSED_BOUND:    return "bound";
    case ACTOR_REFUSED_LOCKOUT:  return "lockout";
    case ACTOR_REFUSED_COOLDOWN: return "cooldown";
    case ACTOR_REFUSED_RATE:     return "rate";
    }
    return "unknown";
}

/* Every plant that currently binds ANY capability of `id` (plants_table.h's
 * per-capability cap_bound[]/cap_dev[]), added to `arr` once each even when
 * a plant binds more than one of this device's capabilities. */
static void add_plant_ids(cJSON *arr, const plants_table_t *plants, const device_id_t *id)
{
    for (int i = 0; i < PLANTS_MAX; i++) {
        const plant_entry_t *p = &plants->p[i];
        if (!p->in_use) continue;
        bool bound = false;
        for (uint8_t c = 0; c < CAPABILITY_COUNT && !bound; c++) {
            if (p->cap_bound[c] && device_id_equal(&p->cap_dev[c], id)) bound = true;
        }
        if (bound) cJSON_AddItemToArray(arr, cJSON_CreateNumber(p->id));
    }
}

cJSON *device_json(const device_entry_t *e, const plants_table_t *plants, uint32_t now_uptime_s)
{
    cJSON *o = cJSON_CreateObject();

    char idbuf[24];
    device_id_format(&e->id, idbuf, sizeof(idbuf));
    cJSON_AddStringToObject(o, "id", idbuf);
    cJSON_AddStringToObject(o, "kind", device_kind_str(e->id.kind));
    cJSON_AddNumberToObject(o, "last_seen_s", age_s(now_uptime_s, e->last_seen_s));
    /* M3 Task 7 (spec §4): "Keys are never returned by any GET -- the API
     * reports only has_key: true|false." bindkey_has() takes the SAME
     * dev_id string device_id_format() just built (bindkey.c's own
     * contract), so this is a cheap, well-defined check for every device
     * kind -- not just BLE/BTHome -- even though only BLE devices can
     * currently have a key set via POST /api/v1/devices/{id}/key. */
    cJSON_AddBoolToObject(o, "has_key", bindkey_has(idbuf));

    if (e->via_node_valid) {
        char node_name[SWARM_NODE_NAME_LEN + 1];
        if (swarm_store_node_name(e->via_node, node_name) && node_name[0] != '\0') {
            cJSON_AddStringToObject(o, "via", node_name);
        } else {
            char macbuf[18];
            via_node_mac_str(macbuf, sizeof(macbuf), e->via_node);
            cJSON_AddStringToObject(o, "via", macbuf);
        }
    } else {
        cJSON_AddNullToObject(o, "via");
    }
    cJSON_AddNumberToObject(o, "rssi", e->best_rssi);

    /* Sensor display names are still mac-keyed in app_config (V1 heritage;
     * generalising the name store to device_id_t is out of this task's
     * scope, see task-6-report.md), so only BLE devices -- whose addr IS a
     * mac -- can have one. */
    char name[33];
    if (e->id.kind == DEV_KIND_BLE && app_config_get_sensor_name(e->id.addr, name) && name[0] != '\0') {
        cJSON_AddStringToObject(o, "name", name);
    } else {
        cJSON_AddNullToObject(o, "name");
    }

    cJSON *caps = cJSON_AddArrayToObject(o, "caps");
    for (uint8_t c = 0; c < CAPABILITY_COUNT; c++) {
        if (!e->caps[c].valid) continue;
        const capability_t *cap = capability_get(c);
        if (!cap) continue;
        cJSON *co = cJSON_CreateObject();
        cJSON_AddNumberToObject(co, "id", c);
        cJSON_AddStringToObject(co, "name", cap->name);
        cJSON_AddStringToObject(co, "unit", cap->unit);
        cJSON_AddNumberToObject(co, "value", capability_decode(c, e->caps[c].raw));
        cJSON_AddNumberToObject(co, "age_s", age_s(now_uptime_s, e->caps[c].updated_s));
        cJSON_AddItemToArray(caps, co);
    }

    cJSON *plant_ids = cJSON_AddArrayToObject(o, "plant_ids");
    if (plants) add_plant_ids(plant_ids, plants, &e->id);

    /* M5a Task 7 (spec §5, amended): the GATT read-status surface --
     * {interval_s, last_read_s, fails, last_error}, added ONLY for devices
     * whose CURRENTLY matched wrapper declares a connect plan. An
     * advertisement-only device (no wrapper matched yet, or its wrapper
     * carries no plan) gets no "gatt" key at all, so nothing about the
     * existing Devices tab shifts for a hub with no GATT sensors
     * (task-7-brief.md's own words).
     *
     * The event-log write spec §5 originally required per attempt was cut
     * in Task 6 (the decoder task's 3072B stack couldn't hold the LittleFS
     * write chain plus the SSE hook's ~2KB frame without breaching the
     * milestone's 9216B free-heap floor -- see task-6-report.md's Critical
     * 2). That makes these four fields the ENTIRE visibility surface for a
     * connect block that never succeeds -- there is nowhere else for it to
     * show up -- which is why last_error rides along here rather than only
     * being logged. */
    int dev_idx = data_core_find_index(&e->id);
    /* The interval comes from the decoder task's memo, NOT from
     * wrapper_exec_plan_get(): this function runs on the httpd task and on
     * the SSE event-loop task, and plan_get() reaches the unlocked wrapper
     * arena, whose eviction path memmove()s bytecode that the decoder task
     * may be executing. See ble_collector_plan_interval_for_device(). */
    uint32_t interval_s = (dev_idx >= 0)
                              ? ble_collector_plan_interval_for_device(dev_idx)
                              : 0;
    if (interval_s > 0) {
        cJSON *g = cJSON_AddObjectToObject(o, "gatt");
        cJSON_AddNumberToObject(g, "interval_s", interval_s);

        /* gatt_sched_last_ok() returns 0 for "never succeeded" (its own doc
         * comment) as well as for the theoretical, practically unreachable
         * case of a success landing at uptime==0 -- a GATT read needs a
         * scan hit, a connect and a discovery first, none of which can
         * complete inside second 0 of boot. Rendered as JSON null (age_s
         * unset), not a fabricated "0s ago" -- devices.jsx's existing
         * fmtAge(null) => "never" path (already used for last_seen_s)
         * picks this up with no new UI logic needed for the null case
         * itself. */
        uint32_t last_ok_s = gatt_sched_last_ok(dev_idx);
        if (last_ok_s == 0) {
            cJSON_AddNullToObject(g, "last_read_s");
        } else {
            cJSON_AddNumberToObject(g, "last_read_s", age_s(now_uptime_s, last_ok_s));
        }

        cJSON_AddNumberToObject(g, "fails", gatt_sched_fail_count(dev_idx));
        /* "" when there is none (gatt_engine_last_error()'s own contract,
         * never NULL) -- including the "read ok, decode emitted nothing"
         * string Task 6 sets without clearing last_ok_s, so a device in
         * that third state shows a stale last_read_s AND a last_error that
         * explains why, rather than looking contradictory. */
        cJSON_AddStringToObject(g, "last_error", gatt_engine_last_error(dev_idx));
    }

    /* M5b Task 11: the actor state and guard surface, added ONLY for a
     * device the actor table actually declares actions for -- an ordinary
     * sensor gets no "actions" key at all, matching "gatt" above's
     * conditional-key convention. dev_idx < 0 (no data_core row -- see the
     * "gatt" block above) can never resolve an actor pair either, so the
     * loop below simply finds nothing and skips the whole key.
     *
     * Reads the actor table's RAM state ONLY, through actor_pair_state()/
     * actor_lockout() (actor.h/actor_table.h, both new in this task) --
     * never the wrapper arena. This function runs on the httpd task AND
     * the SSE event-loop task (this file's own devices_json.h comment);
     * M5a's whole-branch review found exactly this defect here once
     * already (an arena eviction's memmove() racing a running
     * psvm_run()), which is why the actor table is owned in RAM
     * precisely so this route can read scalars instead. ACTION_COUNT (4,
     * action.h) is the whole firmware's action vocabulary -- small enough
     * to probe every id rather than needing a separate "which actions are
     * declared" enumeration accessor. */
    cJSON *actions = NULL;
    bool lockout = false;
    for (uint8_t aid = 0; aid < ACTION_COUNT; aid++) {
        actor_pair_state_t ps;
        if (!actor_pair_state(dev_idx, aid, &ps)) continue;
        if (!actions) {
            actions = cJSON_AddArrayToObject(o, "actions");
            /* Device-level (actor_table.h: lockout is the operator's stop
             * button for the WHOLE device, not per action) -- read once,
             * the same value rendered on every entry below. */
            actor_lockout(dev_idx, &lockout);
        }
        const action_t *a = action_get(aid);
        cJSON *ao = cJSON_CreateObject();
        cJSON_AddNumberToObject(ao, "id", aid);
        cJSON_AddStringToObject(ao, "name", a ? a->name : "?");
        cJSON_AddStringToObject(ao, "param", action_param_str(a ? a->param : ACTION_PARAM_NONE));
        cJSON_AddNumberToObject(ao, "param_max", ps.param_max);
        cJSON_AddNumberToObject(ao, "cooldown_s", ps.cooldown_s);
        cJSON_AddNumberToObject(ao, "max_per_hour", ps.max_per_hour);
        cJSON_AddNumberToObject(ao, "activations_this_hour", ps.activations_this_hour);
        cJSON_AddBoolToObject(ao, "lockout", lockout);
        if (ps.has_fired) {
            cJSON_AddNumberToObject(ao, "last_fired_s", age_s(now_uptime_s, ps.last_fire_s));
        } else {
            cJSON_AddNullToObject(ao, "last_fired_s");
        }
        cJSON_AddStringToObject(ao, "last_result", live_verdict_str(ps.live_verdict));
        cJSON_AddItemToArray(actions, ao);
    }

    return o;
}

cJSON *plant_json(const plant_entry_t *p, const registry_t *reg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);   /* "" = unnamed; UI renders "Plant <id>" */

    cJSON *bindings = cJSON_AddArrayToObject(o, "bindings");
    for (uint8_t c = 0; c < CAPABILITY_COUNT; c++) {
        if (!p->cap_bound[c]) continue;
        const capability_t *cap = capability_get(c);

        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "cap", c);
        cJSON_AddStringToObject(b, "name", cap ? cap->name : "?");
        char devbuf[24];
        device_id_format(&p->cap_dev[c], devbuf, sizeof(devbuf));
        cJSON_AddStringToObject(b, "device", devbuf);

        float value; uint32_t age_s;
        if (plants_cap_value(p->id, c, reg, &value, &age_s)) {
            cJSON_AddNumberToObject(b, "value", value);
            cJSON_AddNumberToObject(b, "age_s", age_s);
        } else {
            cJSON_AddNullToObject(b, "value");
            cJSON_AddNullToObject(b, "age_s");
        }
        cJSON_AddItemToArray(bindings, b);
    }
    return o;
}
