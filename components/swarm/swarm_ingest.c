#include "swarm_ingest.h"
#include <string.h>
#include "data_core.h"
#include "capability.h"
#include "actor.h"
#include "tuya_dp.h"
#include "mapping_engine.h"
#include "rules.h"
#include "esp_log.h"

static const char *TAG = "swarm_ingest";

void swarm_ingest_announce(const swarm_device_announce_t *a, const uint8_t *via_mac, uint32_t now_s)
{
    device_id_t id = { .kind = (device_kind_t)a->dev.kind };
    memcpy(id.addr, a->dev.addr, SWARM_ADDR_LEN);
    int idx = data_core_find_or_create_index(&id, now_s);
    if (idx < 0) return;   /* registry full: already counted/logged by data_core */

    /* via_mac == NULL is the LOCAL radio (the H2 over UART): no node to
     * attribute this device to. The ESP-NOW path passes the forwarding
     * bridge node's MAC so the registry records who relays it. */
    if (via_mac) data_core_set_via(idx, via_mac);

    /* Mirror manufacturer/model only when the announce actually carries at
     * least one, so a re-announce that omits them does not blank an identity
     * captured earlier. decode bounds both lengths to SWARM_DEV_STR_MAX. */
    if (a->manuf_len || a->model_len) {
        char mf[SWARM_DEV_STR_MAX + 1] = { 0 };
        char md[SWARM_DEV_STR_MAX + 1] = { 0 };
        memcpy(mf, a->manufacturer, a->manuf_len);
        memcpy(md, a->model, a->model_len);
        data_core_set_identity(idx, mf, md);
    }

    /* An announce carries the device's FULL current action set: declare each
     * (param_max=0, no flags -- every zigbee action today is parameterless)
     * then prune any previously-declared action now absent. */
    for (uint8_t i = 0; i < a->action_count; i++) {
        if (!actor_declare_ep(idx, a->action_ids[i], a->action_endpoints[i], 0, 0)) {
            ESP_LOGW(TAG, "device %d: could not declare action %u@ep%u",
                     idx, (unsigned)a->action_ids[i], (unsigned)a->action_endpoints[i]);
        }
    }
    actor_prune_absent(idx, a->action_ids, a->action_count);
    actor_set_device_key(idx, (const uint8_t *)&id);
}

void swarm_ingest_device_gone(const swarm_device_gone_t *g)
{
    /* The registry never deletes rows, so the device entry survives -- but
     * its via_node attribution is now stale and must not keep pointing at a
     * radio that no longer reports it. A later re-announce re-attributes. */
    device_id_t id = { .kind = (device_kind_t)g->dev.kind };
    memcpy(id.addr, g->dev.addr, SWARM_ADDR_LEN);
    int idx = data_core_find_index(&id);
    if (idx >= 0) data_core_clear_via(idx);
}

bool swarm_ingest_measurement(const swarm_measurement_t *m, uint32_t extra_age_s, uint32_t now_s)
{
    (void)now_s;
    device_id_t id = { .kind = (device_kind_t)m->dev.kind };
    memcpy(id.addr, m->dev.addr, SWARM_ADDR_LEN);

    /* Unknown device: a reading for something never announced (or whose
     * announce this hub missed). Report it so the caller can request a
     * resync/replay rather than silently dropping forever. */
    if (data_core_find_index(&id) < 0)
        return false;

    /* A standard-cluster reading (source_cluster != 0) whose cluster is in
     * THIS device's suppress-set never reaches data_core -- fed this
     * device's OWN suppress-set, never a global one. source_cluster == 0
     * (Tuya EF00 raw DPs) is never suppressed here. */
    if (m->source_cluster != 0) {
        uint16_t sset[TUYA_SUPPRESS_MAX];
        int sn = tuya_dp_suppress_list(&id, sset, TUYA_SUPPRESS_MAX);
        if (mapping_measurement(m->source_cluster, sset, sn) == MAP_DROP) {
            ESP_LOGD(TAG, "measurement cluster 0x%04x suppressed", m->source_cluster);
            return true;
        }
    }

    /* Honest age: the frame's own transmit-time age plus whatever it spent
     * queued after receipt (extra_age_s). Clamped at UINT16_MAX the same way
     * the producer's own recompute is; submit_cap_id_ep drops anything past
     * DATA_CORE_MAX_AGE_S long before the clamp could matter. */
    uint32_t total_age_s = (uint32_t)m->age_s + extra_age_s;
    uint16_t age_for_data_core = (total_age_s > UINT16_MAX) ? UINT16_MAX : (uint16_t)total_age_s;

    const capability_t *mc = capability_get(m->cap_id);
    bool ok;
    if (mc && mc->event) {
        ok = data_core_submit_event(&id, m->cap_id, (int16_t)m->value);
        ESP_LOGI(TAG, "event cap %u code %d -> %s", m->cap_id, (int)m->value, ok ? "queued" : "dropped");
    } else {
        ok = data_core_submit_cap_id_ep(&id, m->cap_id, m->endpoint, m->value, age_for_data_core);
        ESP_LOGI(TAG, "measurement cap %u ep %u value %.3f age %us -> %s", m->cap_id,
                 (unsigned)m->endpoint, (double)m->value, (unsigned)age_for_data_core,
                 ok ? "accepted" : "rejected");
    }
    if (ok) rules_notify_value_update();
    return true;
}

void swarm_ingest_tuya_dp(const swarm_tuya_dp_t *d, uint32_t now_s)
{
    device_id_t id = { .kind = (device_kind_t)d->dev.kind };
    memcpy(id.addr, d->dev.addr, SWARM_ADDR_LEN);

    /* Discovery, not a registry write: record the raw DP regardless of
     * whether it's mapped yet (not gated on data_core_find_index). */
    tuya_dp_observe(&id, d->dp_id, d->dp_type, d->value, now_s);

    /* Apply THIS device's offset-aware DP-map via the pure engine.
     * tuya_dp_map_list() is a FLAT table across every Tuya device -- filter
     * to this device's own entries first so a dp_id mapped for one device is
     * never misapplied to another reporting the same dp_id. */
    tuya_dp_map_t all[TUYA_MAP_MAX];
    int total = tuya_dp_map_list(all, TUYA_MAP_MAX);
    tuya_dp_map_t applied[TUYA_MAP_MAX];
    int an = 0;
    for (int i = 0; i < total; i++)
        if (device_id_equal(&all[i].id, &id)) applied[an++] = all[i];

    uint8_t cap; float mapped;
    if (mapping_dp(d->dp_id, d->value, applied, an, &cap, &mapped) == MAP_SUBMIT) {
        bool ok = data_core_submit_cap_id(&id, cap, mapped);
        ESP_LOGI(TAG, "tuya dp 0x%02x cap %u -> %s", d->dp_id, cap, ok ? "accepted" : "rejected");
        if (ok) rules_notify_value_update();
    }
    /* else MAP_RECORD_UNMAPPED: already recorded by tuya_dp_observe() above. */
}
