#include "batt_cycle.h"

/* Period table for power modes. Must match the periods implied by the tests:
 * mode 0 (ALWAYS_ON): 0 (never sleeps)
 * mode 1: 15 minutes = 900 seconds
 * mode 2: 60 minutes = 3600 seconds
 */
static const uint32_t period_table[] = {
    0,              /* mode 0: ALWAYS_ON */
    15u * 60u,      /* mode 1: 15 minutes */
    60u * 60u,      /* mode 2: 60 minutes */
};
#define PERIOD_TABLE_LEN (sizeof(period_table) / sizeof(period_table[0]))

uint32_t batt_period_s(uint8_t power_mode)
{
    if (power_mode >= PERIOD_TABLE_LEN) {
        return 0;  /* out of range treated as ALWAYS_ON */
    }
    return period_table[power_mode];
}

batt_cmd_t batt_reconcile(uint8_t desired_mode, uint8_t reported_mode, bool ota_pending)
{
    batt_cmd_t result = {0, 0};

    /* STAY_AWAKE (for OTA) has priority */
    if (ota_pending) {
        result.command = 2;  /* SWARM_CHECKIN_CMD_STAY_AWAKE */
        result.arg = 0;
        return result;
    }

    /* Check if reported mode is valid (0, 1, or 2).
     * Anything else is treated as unknown -> SET_MODE toward desired */
    bool reported_valid = (reported_mode < PERIOD_TABLE_LEN);

    /* If reported is invalid or differs from desired, send SET_MODE */
    if (!reported_valid || reported_mode != desired_mode) {
        result.command = 1;  /* SWARM_CHECKIN_CMD_SET_MODE */
        result.arg = desired_mode;
        return result;
    }

    /* Modes match and no OTA -> NONE */
    result.command = 0;  /* SWARM_CHECKIN_CMD_NONE */
    result.arg = 0;
    return result;
}

uint64_t batt_sleep_us(uint8_t power_mode, uint32_t awake_ms)
{
    uint32_t period_s = batt_period_s(power_mode);
    uint32_t awake_s = awake_ms / 1000u;

    /* Calculate sleep time in seconds */
    uint32_t sleep_s;
    if (awake_s >= period_s) {
        /* Over-long wake: minimum 10 seconds */
        sleep_s = 10u;
    } else {
        sleep_s = period_s - awake_s;
    }

    /* Convert to microseconds */
    return (uint64_t)sleep_s * 1000000u;
}

uint32_t batt_failed_wake_next(uint32_t counter, bool wake_succeeded, bool *fallback)
{
    if (wake_succeeded) {
        *fallback = false;
        return 0;
    }

    /* Wake failed: increment counter */
    uint32_t next_counter = counter + 1u;

    /* Check if we've hit the limit */
    if (next_counter >= BATT_FAILED_WAKE_LIMIT) {
        *fallback = true;
        return BATT_FAILED_WAKE_LIMIT;
    }

    *fallback = false;
    return next_counter;
}
