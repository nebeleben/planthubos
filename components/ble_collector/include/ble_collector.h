#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ble_collector_start(void);

/* Advertisements dropped because the raw-advert queue (M3 spec §1) was
 * full when gap_event tried to push -- exposed at GET /api/v1/status as
 * "adv_dropped" (api_v1.c) so a saturated queue is visible, not silent. */
uint32_t ble_collector_adv_dropped(void);
