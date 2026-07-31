#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t claim_init(void);
bool      claim_is_claimed(void);
esp_err_t claim_generate(char secret_hex[65]);
bool      claim_verify(const char *secret_hex);
esp_err_t claim_reset(void);
void      factory_reset_button_start(void);
