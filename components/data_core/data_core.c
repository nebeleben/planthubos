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

void data_core_submit(const mibeacon_t *m)
{
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = registry_update(&s_registry, m, now_s);
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
