#pragma once
#include "esp_err.h"
/* Starts MQTT and/or Influx per the stored config. Call once, from main.c's
 * hub branch only, AFTER wifi_manager_start() (needs the netif up) and
 * after integr_config_init(). Safe no-op when both integrations are
 * disabled.
 *
 * Implemented in mqtt_pub.c (the natural owner of "config in, integrations
 * up"): it reads the config once, starts the esp-mqtt client itself when
 * mqtt.enabled, then always calls influx_start() (integr_private.h),
 * which applies its own influx.enabled check the same way. */
esp_err_t integrations_start(void);
