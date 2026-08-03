#pragma once
#include "esp_http_server.h"

esp_err_t ota_post_handler(httpd_req_t *req);

/* Confirms an OTA'd image once the network comes up, so the bootloader does
 * not roll it back. Must be called before wifi is started, so the AP_START /
 * GOT_IP handlers are registered in time. No-op on a serial-flashed image.
 * Hub criteria only -- unchanged by the node-side variant below. */
void ota_rollback_guard_start(void);

/* Node-side variant (M5c): a paired node runs radio-only ESP-NOW and never
 * brings up an AP or associates as a station (see swarm.c's
 * radio_only_wifi_start()), so it has neither of the two events
 * ota_rollback_guard_start() above waits for -- an OTA'd node would boot in
 * PENDING_VERIFY, never confirm, and silently roll back to its old firmware
 * on its next reboot. Call this INSTEAD of ota_rollback_guard_start() from
 * main.c's node_paired boot branch, BEFORE swarm_start_node(). It performs
 * the same PENDING_VERIFY check (a no-op if the running image isn't
 * pending) but registers no WiFi/IP event handlers; confirmation instead
 * happens later via ota_rollback_guard_node_confirm(), which the caller
 * must wire to swarm's node-health signal (see swarm_node_set_health_cb()
 * in swarm.h) -- this component deliberately does not call into swarm.c
 * directly, since webserver already depends on swarm (api_v1.c) and the
 * reverse dependency would be circular; main.c, which depends on both,
 * does the wiring instead. */
void ota_rollback_guard_start_node(void);

/* Confirms an OTA'd NODE image once the caller has proven the node
 * healthy, per the plan: "successfully delivered a reading to its hub" (or
 * received a PONG). Idempotent -- safe to call repeatedly, including once
 * already confirmed (a cheap no-op past the first call) -- and safe to call
 * from any task. Callers on the ESP-NOW receive callback path must NOT
 * call this directly: it performs a flash write (otadata). `reason` is
 * used only for the confirmation log line. No-op if
 * ota_rollback_guard_start_node() was never called, or found nothing
 * pending. */
void ota_rollback_guard_node_confirm(const char *reason);
