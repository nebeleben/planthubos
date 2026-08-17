/* Component-private interface between ble_collector.c and battery_poll.c.
 * Deliberately not in include/ -- nothing outside components/ble_collector
 * should ever call either of these; the two .c files pull it in via a
 * quoted include, resolved relative to this directory. Previously each side
 * declared the other's entry point with a bare `extern` and a comment
 * explaining why there's no header; this file replaces both comments with
 * one that can't drift out of sync between the two callers. */
#pragma once
#include <stdbool.h>
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

/* Implemented in ble_collector.c; resumes the passive scan a poll or GATT
 * attempt paused for its connect/read/terminate cycle. Called from
 * battery_poll.c after a poll attempt ends (success, failure, timeout,
 * disconnect), and injected into the M5a GATT engine
 * (gatt_engine_set_scan_resume()).
 *
 * Returns true iff scanning is actually running when it returns -- see
 * start_scan()'s own comment for why a failure here is permanent, invisible
 * deafness rather than a log line, and what the GATT engine does with a
 * false. */
bool ble_collector_resume_scan(void);

/* Implemented in battery_poll.c; true while a battery poll owns the hub's
 * single outbound connection (CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 1).
 * Injected into the M5a GATT engine (gatt_engine_set_conn_busy_hook()) so
 * the two independent connect-out schedulers stop colliding: without it the
 * loser's ble_gap_connect() simply fails, and the GATT side would record a
 * failure, a backoff and a handle-cache drop against a device that did
 * nothing wrong. The GATT side is visible to battery_poll.c in the other
 * direction through gatt_engine_busy(), which handle_tick() checks. */
bool battery_poll_busy(void);
