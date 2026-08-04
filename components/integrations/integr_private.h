#pragma once
#include "esp_err.h"
#include "integr_config.h"

/* Cross-file interface internal to THIS component only (deliberately not in
 * include/ -- nothing outside components/integrations should ever call
 * this directly). integrations_start() (mqtt_pub.c) is the natural owner
 * of "read config once, start what's enabled": it starts MQTT itself, then
 * unconditionally calls influx_start(cfg), which is responsible for its
 * own off-by-default check (influx.enabled) exactly the way mqtt_pub.c
 * handles mqtt.enabled. Stub in influx.c for now (always ESP_OK, does
 * nothing); Task 5 replaces it with the real InfluxDB HTTP write client. */
esp_err_t influx_start(const integr_config_t *cfg);
