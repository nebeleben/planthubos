/* Filled by Task 5 (InfluxDB HTTP write client). Stub for now: always
 * succeeds and does nothing, so integrations_start() (mqtt_pub.c) can call
 * it unconditionally regardless of influx.enabled -- that check (and the
 * real client) lands in Task 5. */
#include "integr_private.h"

esp_err_t influx_start(const integr_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}
