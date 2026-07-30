#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_manager_start(void);
void      wifi_manager_apply_new_creds(void);
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_get_ip(char out[16]);
