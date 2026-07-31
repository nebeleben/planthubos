#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

/* Posted (with a uint32_t epoch_s payload) from the SNTP sync callback,
 * which runs on the lwip/tcpip task, onto the default event loop -- so the
 * boottab write in timekeeper_set_epoch never stalls network I/O. */
ESP_EVENT_DECLARE_BASE(PLANTHUB_TIME_EVENT);
enum { TIME_EVENT_EPOCH_LEARNED };

esp_err_t timekeeper_init(const char *base_path);
uint16_t  timekeeper_boot_id(void);
void      timekeeper_set_epoch(uint32_t epoch_s);
bool      timekeeper_resolve(uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
bool      timekeeper_synced(void);
uint32_t  timekeeper_now(void);
