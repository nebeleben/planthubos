#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_sched.h"

int main(void)
{
    batt_entry_t tab[BATT_MAX_SENSORS];
    memset(tab, 0, sizeof tab);
    uint8_t mac[6] = {1,2,3,4,5,6}, addr[6] = {6,5,4,3,2,1};
    uint8_t mac2[6] = {9,9,9,9,9,9};

    /* empty table: nothing due */
    assert(batt_sched_pick(tab, 1000) == -1);

    /* seen -> due immediately (never polled) */
    batt_sched_seen(tab, mac, 0, addr, 1000);
    assert(tab[0].in_use && tab[0].addr_val[0] == 6);
    assert(batt_sched_pick(tab, 1000) == 0);

    /* stale sighting (>300s) -> not due */
    assert(batt_sched_pick(tab, 1400) == -1);

    /* re-seen refreshes, same slot (no duplicate) */
    batt_sched_seen(tab, mac, 0, addr, 1500);
    assert(!tab[1].in_use);
    assert(batt_sched_pick(tab, 1500) == 0);

    /* failed attempt backs off for BATT_RETRY_INTERVAL_S */
    tab[0].last_attempt_s = 1500;
    assert(batt_sched_pick(tab, 1600) == -1);
    batt_sched_seen(tab, mac, 0, addr, 1500 + BATT_RETRY_INTERVAL_S);
    assert(batt_sched_pick(tab, 1500 + BATT_RETRY_INTERVAL_S) == 0);

    /* success backs off for BATT_POLL_INTERVAL_S */
    tab[0].last_ok_s = 5000; tab[0].last_attempt_s = 5000;
    batt_sched_seen(tab, mac, 0, addr, 6000);
    assert(batt_sched_pick(tab, 6000) == -1);
    uint32_t due = 5000 + BATT_POLL_INTERVAL_S;
    batt_sched_seen(tab, mac, 0, addr, due);
    assert(batt_sched_pick(tab, due) == 0);

    /* most-overdue wins: never-polled beats polled-long-ago */
    batt_sched_seen(tab, mac2, 0, addr, due);
    assert(batt_sched_pick(tab, due) == 1);

    printf("test_battery_sched: OK\n");
    return 0;
}
