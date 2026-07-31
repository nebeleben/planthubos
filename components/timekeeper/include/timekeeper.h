#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t timekeeper_init(const char *base_path);
uint16_t  timekeeper_boot_id(void);
void      timekeeper_set_epoch(uint32_t epoch_s);
bool      timekeeper_resolve(uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
bool      timekeeper_synced(void);
uint32_t  timekeeper_now(void);
