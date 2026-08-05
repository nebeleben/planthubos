/* Component-private interface between ble_collector.c and battery_poll.c.
 * Deliberately not in include/ -- nothing outside components/ble_collector
 * should ever call either of these; the two .c files pull it in via a
 * quoted include, resolved relative to this directory. Previously each side
 * declared the other's entry point with a bare `extern` and a comment
 * explaining why there's no header; this file replaces both comments with
 * one that can't drift out of sync between the two callers. */
#pragma once
#include "battery_sched.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Implemented in battery_poll.c; starts the 60s poll-kickoff timer and
 * poller task operating on tab, guarding every access to it with
 * batt_mutex. Called from ble_collector.c's ble_collector_start(), which
 * creates batt_mutex before nimble_port_freertos_init() so it exists before
 * gap_event could possibly run. Hub-only -- ble_collector.c skips this call
 * on a node (battery polling connects out and would fight ESP-NOW timing on
 * the node's single radio). */
void battery_poll_start(batt_entry_t *tab, SemaphoreHandle_t batt_mutex);

/* Implemented in ble_collector.c; resumes the passive scan a poll attempt
 * paused for its connect/read/terminate cycle. Called from battery_poll.c
 * after a poll attempt ends (success, failure, timeout, disconnect). */
void ble_collector_resume_scan(void);
