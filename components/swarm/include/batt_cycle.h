#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BATT_SCAN_WINDOW_S        30u
#define BATT_CHECKIN_WAIT_MS      2000u
#define BATT_FAILED_WAKE_LIMIT    20u
#define BATT_STAY_AWAKE_CAP_S     (12u * 60u)
#define BATT_ALWAYS_ON_CHECKIN_S  (5u * 60u)

/* Period in seconds for a mode; 0 for ALWAYS_ON (=never sleeps). */
uint32_t batt_period_s(uint8_t power_mode /* SWARM_PM_* as u8 */);

/* Hub side: one command per checkin (spec §6). STAY_AWAKE (ota_pending)
 * takes priority over SET_MODE; equal modes and no OTA -> NONE.
 * desired/reported are SWARM_PM_* values; an out-of-range reported value
 * is treated as unknown -> SET_MODE toward desired.
 *
 * Command enum values: 0=NONE, 1=SET_MODE, 2=STAY_AWAKE.
 * These MUST match SWARM_CHECKIN_CMD_* from swarm_frame.h and must never drift. */
typedef struct { uint8_t command; uint8_t arg; } batt_cmd_t;
batt_cmd_t batt_reconcile(uint8_t desired_mode, uint8_t reported_mode, bool ota_pending);

/* Node side: microseconds to deep-sleep after a wake that consumed
 * awake_ms. Never returns less than 10 seconds' worth (a pathological
 * over-long wake must not busy-loop the node). */
uint64_t batt_sleep_us(uint8_t power_mode, uint32_t awake_ms);

/* Node side: failed-wake bookkeeping (spec §4). Given the persisted
 * counter and this wake's outcome, returns the new counter value and sets
 * *fallback if the ALWAYS_ON fallback must trigger now. */
uint32_t batt_failed_wake_next(uint32_t counter, bool wake_succeeded, bool *fallback);
