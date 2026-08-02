#pragma once
#include "esp_http_server.h"

esp_err_t ota_post_handler(httpd_req_t *req);

/* Confirms an OTA'd image once the network comes up, so the bootloader does
 * not roll it back. Must be called before wifi is started, so the AP_START /
 * GOT_IP handlers are registered in time. No-op on a serial-flashed image. */
void ota_rollback_guard_start(void);
