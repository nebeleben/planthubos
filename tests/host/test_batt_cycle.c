#include <assert.h>
#include <stdio.h>
#include "batt_cycle.h"

int main(void)
{
    /* periods */
    assert(batt_period_s(0) == 0);
    assert(batt_period_s(1) == 15u * 60u);
    assert(batt_period_s(2) == 60u * 60u);

    /* reconcile: agreement, no ota -> NONE */
    batt_cmd_t c = batt_reconcile(1, 1, false);
    assert(c.command == 0 /* NONE */);
    /* mode differs -> SET_MODE toward desired */
    c = batt_reconcile(2, 1, false);
    assert(c.command == 1 && c.arg == 2);
    c = batt_reconcile(0, 2, false);
    assert(c.command == 1 && c.arg == 0);
    /* ota pending beats everything */
    c = batt_reconcile(2, 1, true);
    assert(c.command == 2 /* STAY_AWAKE */);
    /* garbage reported -> treated unknown -> SET_MODE */
    c = batt_reconcile(1, 77, false);
    assert(c.command == 1 && c.arg == 1);

    /* sleep arithmetic: 15min period minus 35s awake */
    assert(batt_sleep_us(1, 35000) == (uint64_t)(15u*60u - 35u) * 1000000u);
    /* floor: awake longer than the period still sleeps >= 10s */
    assert(batt_sleep_us(1, 20u*60u*1000u) == 10u * 1000000u);
    /* 60min mode */
    assert(batt_sleep_us(2, 35000) == (uint64_t)(60u*60u - 35u) * 1000000u);

    /* failed-wake counter */
    bool fb = true;
    assert(batt_failed_wake_next(5, true, &fb) == 0 && !fb);   /* success clears */
    fb = false;
    assert(batt_failed_wake_next(0, false, &fb) == 1 && !fb);
    assert(batt_failed_wake_next(18, false, &fb) == 19 && !fb);
    assert(batt_failed_wake_next(19, false, &fb) == 20 && fb); /* 20th trips */

    printf("test_batt_cycle: OK\n");
    return 0;
}
