#include "data_core.h"
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

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = registry_find(&s_registry, m->mac);
    if (idx >= 0 && effective_s < s_registry.sensors[idx].last_seen_s) {
        /* A buffered reading arriving late must not regress the live view
         * behind a value we already have that is newer. This is a
         * "don't overwrite newer with older" guard only -- it does not
         * insert into an earlier history slot; see the age policy note in
         * data_core.h. */
        uint32_t stored_s = s_registry.sensors[idx].last_seen_s;
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "dropping stale " MACSTR_FMT ": effective %us < last_seen %us (dropped_stale=%lu)",
                 MAC_ARG(m->mac), (unsigned)effective_s, (unsigned)stored_s,
                 (unsigned long)++s_dropped_stale);
        return;
    }
    int rc = registry_update_from(&s_registry, m, effective_s, via_node, rssi);
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

void data_core_submit(const mibeacon_t *m)
{
    data_core_submit_from(m, NULL, 0, 0);
}

void data_core_snapshot(registry_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_registry, sizeof(*out));
    xSemaphoreGive(s_mutex);
}
