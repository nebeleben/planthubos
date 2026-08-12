#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

esp_err_t app_config_init(void);
bool      app_config_get_wifi(wifi_creds_t *out);
esp_err_t app_config_set_wifi(const wifi_creds_t *c);   /* ESP_ERR_INVALID_ARG if invalid */
esp_err_t app_config_clear_wifi(void);
void      app_config_hub_name(char out[16]);            /* operator-set name, else "PlantHub-XXXX" */
esp_err_t app_config_set_hub_name(const char *name);    /* [A-Za-z0-9_-]{0,15}; "" = back to default */
esp_err_t app_config_set_sensor_name(const uint8_t mac[6], const char *name);
bool      app_config_get_sensor_name(const uint8_t mac[6], char out[33]);
esp_err_t app_config_clear_sensor_name(const uint8_t mac[6]);   /* erase; == set_sensor_name(mac, "") */

/* Rapid power-cycle WiFi reset (see power_reset.c). Call once from
 * app_main(), immediately after app_config_init() -- the increment must
 * land before any slow init so a user yanking power every ~2 seconds
 * still advances the counter. */
void power_cycle_reset_start(void);
