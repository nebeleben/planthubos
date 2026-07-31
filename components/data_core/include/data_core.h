#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include "registry.h"

ESP_EVENT_DECLARE_BASE(PLANTHUB_DATA_EVENT);
enum { DATA_EVENT_SENSOR_UPDATE };

esp_err_t data_core_init(void);
void      data_core_submit(const mibeacon_t *m);
void      data_core_snapshot(registry_t *out);
