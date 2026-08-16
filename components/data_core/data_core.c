#include "data_core.h"
#include "capability.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define MACSTR_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARG(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

ESP_EVENT_DEFINE_BASE(PLANTHUB_DATA_EVENT);

static const char *TAG = "data_core";
static registry_t s_registry;
static SemaphoreHandle_t s_mutex;

esp_err_t data_core_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    registry_init(&s_registry);
    return ESP_OK;
}

static uint32_t s_dropped_stale;

/* Must be called with s_mutex held. Encodes value into cap_id and writes it
 * via registry_set_cap() -- UNLESS capability_encode() returns
 * CAP_VALUE_NONE for an out-of-range value (e.g. MiFlora's uint16
 * conductivity_us can exceed CAP_SOIL_CONDUCTIVITY's int16 ceiling), in
 * which case the registry_set_cap() call is skipped entirely and a WARN is
 * logged instead. This is deliberately NOT "pass CAP_VALUE_NONE through" --
 * that is registry_set_cap()'s own, distinct "caller explicitly wants this
 * slot cleared" contract (still exercised directly by callers that mean it,
 * e.g. a device losing a capability outright), and reusing it here would
 * silently ERASE a previously-good stored reading just because one later
 * frame happened to carry a bad value, with no diagnostic. Skipping the
 * call instead leaves whatever was already stored untouched. */
static void set_cap_or_warn(const device_id_t *id, uint8_t cap_id, float value, uint32_t ts_s)
{
    int16_t raw = capability_encode(cap_id, value);
    if (raw == CAP_VALUE_NONE) {
        const capability_t *c = capability_get(cap_id);
        ESP_LOGW(TAG, "%s: value %.2f out of range, dropping this reading (previous value kept)",
                 c ? c->name : "?", (double)value);
        return;
    }
    registry_set_cap(&s_registry, id, cap_id, raw, ts_s);
}

/* Must be called with s_mutex held. The MiFlora -> capability adapter: runs
 * registry_attribute() first (M5b arbitration), and writes capability
 * values only when this reporter actually owns the frame -- the same
 * "attribute first, then write values only if we own this frame" ordering
 * V1's registry_update_from() followed. Returns 1 when values were written
 * (post DATA_EVENT_SENSOR_UPDATE), 0 when this reporter lost the
 * arbitration (nothing written, nothing to post), -1 when the registry is
 * full and the device is unknown. */
static int submit_locked(const mibeacon_t *m, const device_id_t *id,
                          const uint8_t via_node[6], int8_t rssi, uint32_t ts_s)
{
    bool own = registry_attribute(&s_registry, id, m->frame_cnt, via_node, rssi, ts_s);
    int idx = registry_find(&s_registry, id);
    if (idx < 0) return -1;
    if (!own) return 0;

    if (m->has_temp)         set_cap_or_warn(id, CAP_AIR_TEMPERATURE, m->temp_dc / 10.0f, ts_s);
    if (m->has_moisture)     set_cap_or_warn(id, CAP_SOIL_MOISTURE, (float)m->moisture_pct, ts_s);
    if (m->has_lux)          set_cap_or_warn(id, CAP_LIGHT_ILLUMINANCE, (float)m->lux, ts_s);
    if (m->has_conductivity) set_cap_or_warn(id, CAP_SOIL_CONDUCTIVITY, (float)m->conductivity_us, ts_s);
    if (m->has_battery)      set_cap_or_warn(id, CAP_BATTERY_LEVEL, (float)m->battery_pct, ts_s);
    /* The winning reporter's own signal strength, always -- not gated on
     * any has_* flag, since it describes the RADIO link, not a sensor
     * reading the MiBeacon frame carried. */
    set_cap_or_warn(id, CAP_SIGNAL_RSSI, (float)rssi, ts_s);
    return 1;
}

void data_core_submit_mibeacon(const mibeacon_t *m, const uint8_t via_node[6], int8_t rssi)
{
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    device_id_t id = device_id_from_mac(DEV_KIND_BLE, m->mac);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = submit_locked(m, &id, via_node, rssi, now_s);
    xSemaphoreGive(s_mutex);

    if (rc < 0) {
        ESP_LOGW(TAG, "registry full, dropping " MACSTR_FMT, MAC_ARG(m->mac));
        return;
    }
    if (rc == 1) {
        /* mac is copied into the event queue by esp_event */
        esp_event_post(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE,
                       (void *)m->mac, 6, 0 /* don't block the caller's task */);
    }
}

void data_core_submit_from(const mibeacon_t *m, const uint8_t via_node[6],
                            int8_t rssi, uint16_t age_s)
{
    if (age_s > DATA_CORE_MAX_AGE_S) {
        ESP_LOGD(TAG, "dropping " MACSTR_FMT ": age %us exceeds max %us (dropped_stale=%lu)",
                 MAC_ARG(m->mac), (unsigned)age_s, (unsigned)DATA_CORE_MAX_AGE_S,
                 (unsigned long)++s_dropped_stale);
        return;
    }

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    /* age_s back-dates the effective capture time; clamp rather than
     * underflow if a node ever reports an age older than our own uptime. */
    uint32_t effective_s = (age_s <= now_s) ? now_s - age_s : 0;
    device_id_t id = device_id_from_mac(DEV_KIND_BLE, m->mac);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = registry_find(&s_registry, &id);
    if (idx >= 0 && effective_s < s_registry.devices[idx].last_seen_s) {
        /* A buffered reading arriving late must not regress the live view
         * behind a value we already have that is newer. This is a
         * "don't overwrite newer with older" guard only -- it does not
         * insert into an earlier history slot; see the age policy note in
         * data_core.h. */
        uint32_t stored_s = s_registry.devices[idx].last_seen_s;
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "dropping stale " MACSTR_FMT ": effective %us < last_seen %us (dropped_stale=%lu)",
                 MAC_ARG(m->mac), (unsigned)effective_s, (unsigned)stored_s,
                 (unsigned long)++s_dropped_stale);
        return;
    }
    int rc = submit_locked(m, &id, via_node, rssi, effective_s);
    xSemaphoreGive(s_mutex);

    if (rc < 0) {
        ESP_LOGW(TAG, "registry full, dropping " MACSTR_FMT, MAC_ARG(m->mac));
        return;
    }
    if (rc == 1) {
        /* mac is copied into the event queue by esp_event */
        esp_event_post(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE,
                       (void *)m->mac, 6, 0 /* don't block the BLE host task */);
    }
}

void data_core_snapshot(registry_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_registry, sizeof(*out));
    xSemaphoreGive(s_mutex);
}

/* Single-device lookup -- see data_core.h's doc comment. Deliberately NOT
 * implemented as "take a full registry_t snapshot via data_core_snapshot(),
 * then registry_find() into that" (which would be simpler): several callers
 * (webserver/sse.c, swarm.c's on_sensor_update()) run on the default
 * event-loop task, which both files' own comments document as having only
 * ~2304 bytes of stack; a 2048-byte registry_t local on top of that call
 * chain would risk overflowing it. Bounded, allocation-free -- same
 * critical-section cost shape as data_core_snapshot()'s own memcpy. */
bool data_core_get_device(const device_id_t *id, device_entry_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = registry_find(&s_registry, id);
    bool found = idx >= 0;
    if (found) memcpy(out, &s_registry.devices[idx], sizeof(*out));
    xSemaphoreGive(s_mutex);
    return found;
}

void data_core_clear_node_attribution(const uint8_t node_mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    registry_clear_attribution(&s_registry, node_mac);
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "cleared via-node attribution for " MACSTR_FMT, MAC_ARG(node_mac));
}

bool data_core_submit_battery(const uint8_t mac[6], uint8_t pct)
{
    /* capability_encode() is pure (no shared state), so this check happens
     * before taking s_mutex. Same "skip the write, don't pass
     * CAP_VALUE_NONE through" guard as set_cap_or_warn() above -- see its
     * comment. pct is a uint8_t (0-100) and CAP_BATTERY_LEVEL's scale/offset
     * give it an int16 ceiling, so this can never actually trigger today;
     * checked anyway for the same reason every other capability write is
     * checked, rather than leaving one submit path silently exempt. */
    int16_t raw = capability_encode(CAP_BATTERY_LEVEL, (float)pct);
    if (raw == CAP_VALUE_NONE) {
        ESP_LOGW(TAG, "battery.level: value %u out of range, dropping reading for "
                 MACSTR_FMT " (previous value kept)", (unsigned)pct, MAC_ARG(mac));
        return false;
    }

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    device_id_t id = device_id_from_mac(DEV_KIND_BLE, mac);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = registry_set_cap(&s_registry, &id, CAP_BATTERY_LEVEL, raw, now_s);
    xSemaphoreGive(s_mutex);

    if (idx < 0) {
        ESP_LOGW(TAG, "registry full, dropping battery reading for " MACSTR_FMT, MAC_ARG(mac));
        return false;
    }
    /* mac is copied into the event queue by esp_event, same as
     * data_core_submit_from()'s post on a merge. */
    esp_event_post(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE,
                   (void *)mac, 6, 0 /* don't block the battery poller task */);
    return true;
}
