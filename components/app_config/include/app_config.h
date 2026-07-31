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
void      app_config_hub_name(char out[16]);            /* "PlantHub-XXXX" */
esp_err_t app_config_set_sensor_name(const uint8_t mac[6], const char *name);
bool      app_config_get_sensor_name(const uint8_t mac[6], char out[33]);
