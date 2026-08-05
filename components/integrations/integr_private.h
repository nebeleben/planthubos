#pragma once
#include "esp_err.h"
#include "integr_config.h"

/* Cross-file interface internal to THIS component only (deliberately not in
 * include/ -- nothing outside components/integrations should ever call
 * this directly). integrations_start() (mqtt_pub.c) is the natural owner
 * of "read config once, start what's enabled": it starts MQTT itself, then
 * unconditionally calls influx_start(cfg), which is responsible for its
 * own off-by-default check (influx.enabled) exactly the way mqtt_pub.c
 * handles mqtt.enabled. Implemented in influx.c: spawns a task that POSTs
 * batched InfluxDB v2 line protocol every 300s. */
esp_err_t influx_start(const integr_config_t *cfg);
