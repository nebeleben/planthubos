#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    struct {
        bool enabled;
        char uri[97];      /* "mqtt://192.168.1.10:1883" — esp-mqtt URI form */
        char user[33];     /* "" = anonymous */
        char pass[49];
    } mqtt;
    struct {
        bool enabled;
        char url[97];      /* base URL, e.g. "http://192.168.1.10:8086" */
        char org[33];
        char bucket[33];
        char token[129];
    } influx;
} integr_config_t;

esp_err_t integr_config_init(void);                        /* load NVS → RAM cache; missing key = all-defaults */
void      integr_config_get(integr_config_t *out);         /* RAM cache copy, mutex-guarded */
esp_err_t integr_config_set(const integr_config_t *c);     /* validate, cache, persist. ESP_ERR_INVALID_ARG on bad input */
