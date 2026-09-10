#pragma once
/* Host-test shim (Task 4, zigbee-button-support): data_core.c/.h are the
 * first data_core-side files a host test links IN FULL (every other
 * data_core host test -- test_registry.c -- links only registry.c/
 * capability.c/device_id.c, all of which are already ESP-IDF-free). This
 * repo's established way to keep a file host-testable is an `#ifdef
 * ESP_PLATFORM` gate around the real ESP-IDF calls (see actor.c, or
 * data_fmt.h's own comment on why it has "zero ESP-IDF dependencies");
 * doing that retroactively to every pre-existing function in data_core.c
 * (submit_mibeacon, submit_battery, submit_cap_id_at, ...) would be a much
 * larger, riskier diff than this task's brief calls for. This directory
 * instead provides just enough of a handful of ESP-IDF headers -- esp_err.h,
 * esp_event.h, esp_log.h, esp_timer.h, freertos/FreeRTOS.h,
 * freertos/semphr.h -- for data_core.c to compile and link with plain `cc`,
 * matching the real ESP-IDF declarations closely enough that nothing in
 * data_core.c needed to change to use them. Only reachable via this
 * directory's own -I in run.sh (added before the real include paths would
 * ever matter here, since a plain `cc` host build never sees the ESP-IDF
 * tree at all) -- the device build is untouched and still uses the real
 * ESP-IDF headers exclusively. */
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_NO_MEM  0x101
