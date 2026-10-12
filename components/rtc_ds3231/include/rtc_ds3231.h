/* rtc_ds3231.h -- optional DS3231 I2C real-time clock on the C6 (SDA=GPIO6,
 * SCL=GPIO7, addr 0x68). Gives the hub battery-backed wall-clock time at
 * boot, before WiFi/SNTP; absent-tolerant (no module fitted -> the hub
 * falls back to SNTP exactly as before). Gated by CONFIG_PLANTHUB_RTC_DS3231.
 *
 * Split the PlantHub way: a PURE core (rtc_ds3231_core.c, no ESP-IDF, host-
 * tested) does the DS3231-register <-> epoch conversion and validity; the
 * device layer (rtc_ds3231.c) does the I2C and the event wiring. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* A DS3231 whose year reads below this is treated as never-set / invalid --
 * a fresh or power-lost chip powers up at 2000-01-01, which must not seed a
 * bogus wall clock. 2024 predates this firmware, so any genuinely-set chip
 * reads at or above it. */
#define RTC_DS3231_MIN_YEAR 2024

/* ---- Pure core (host-tested) ---- */

/* Decode the 7 timekeeping registers (0x00 Seconds .. 0x06 Year) plus the
 * oscillator-stop flag (status register 0x0F bit 7) into UTC epoch seconds.
 * Returns false -- leaving *epoch_out untouched -- when OSF is set (the clock
 * lost power), any field is out of range, or the year is < RTC_DS3231_MIN_YEAR.
 * Assumes 24-hour mode and the 2000-2099 century. */
bool rtc_ds3231_decode(const uint8_t regs[7], bool osf, uint32_t *epoch_out);

/* Encode UTC epoch seconds into the 7 timekeeping registers (24-hour mode,
 * century bit 0, day-of-week computed 1=Sunday..7=Saturday). */
void rtc_ds3231_encode(uint32_t epoch, uint8_t regs[7]);

/* ---- Device layer (ESP-IDF; no-ops when the feature is disabled) ---- */

/* Bring up the I2C bus + DS3231 and probe for its ACK, and register the
 * EPOCH_LEARNED handler that trains the chip whenever an authoritative time
 * is learned (SNTP, or a browser time-set). Returns ESP_OK when a DS3231
 * answers, ESP_ERR_NOT_FOUND when nothing acks (no module fitted),
 * ESP_ERR_NOT_SUPPORTED when built disabled, else an I2C error. */
esp_err_t rtc_ds3231_init(void);

/* Read the chip's current time. False when the chip is absent, the I2C read
 * fails, or the stored time is invalid (OSF / implausible year). */
bool rtc_ds3231_read_epoch(uint32_t *epoch_out);

/* Write epoch to the chip (trains it) and clear OSF. False on absence/error. */
bool rtc_ds3231_write_epoch(uint32_t epoch);
