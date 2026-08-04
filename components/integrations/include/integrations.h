#pragma once
#include "esp_err.h"
/* Starts MQTT and/or Influx per the stored config. Call once, from main.c's
 * hub branch only, AFTER wifi_manager_start() (needs the netif up) and
 * after integr_config_init(). Safe no-op when both integrations are
 * disabled. */
esp_err_t integrations_start(void);
