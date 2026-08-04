/* battery_sched.h — no ESP includes */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BATT_MAX_SENSORS   16
#define BATT_POLL_INTERVAL_S (24u * 3600u)   /* per sensor */
#define BATT_RETRY_INTERVAL_S (3600u)        /* after a failed attempt */

typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint8_t  addr_type;        /* NimBLE ble_addr_t.type captured at scan time */
    uint8_t  addr_val[6];      /* ble_addr_t.val — NOT necessarily mac byte order */
    uint32_t last_seen_s;      /* uptime seconds, updated on every direct sighting */
    uint32_t last_ok_s;        /* 0 = never polled successfully */
    uint32_t last_attempt_s;   /* 0 = never attempted */
} batt_entry_t;

/* Record/update a directly-heard sensor (called from the scan path). */
void batt_sched_seen(batt_entry_t *tab, const uint8_t mac[6],
                     uint8_t addr_type, const uint8_t addr_val[6], uint32_t now_s);

/* Pick the most-overdue entry that is due at now_s, or -1.
 * Due = (last_ok_s == 0 || now_s - last_ok_s >= BATT_POLL_INTERVAL_S)
 *   AND (last_attempt_s == 0 || now_s - last_attempt_s >= BATT_RETRY_INTERVAL_S)
 *   AND (now_s - last_seen_s <= 300)   // heard directly in the last 5 min
 * Most-overdue = smallest last_ok_s (never-polled entries, last_ok_s==0, win). */
int  batt_sched_pick(const batt_entry_t *tab, uint32_t now_s);
