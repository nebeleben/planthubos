#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

/* Custom event base used to serialize wifi_manager_apply_new_creds() onto
 * the default event loop task, so it never races on_wifi_event (also driven
 * from the default event loop task) touching the same FSM state. */
ESP_EVENT_DECLARE_BASE(PLANTHUB_EVENT);

enum {
    PLANTHUB_EVENT_APPLY_CREDS,
};

esp_err_t wifi_manager_start(void);
void      wifi_manager_apply_new_creds(void);
/* Zigbee permit-join support (radio-role work, 2026-09-03). Measured with a
 * sniffer: with WiFi/802.15.4 coex arbitration on -- required for a usable
 * LAN in the zigbee role -- the coordinator still answers beacon requests,
 * but never gets the antenna back fast enough to send the MAC ACK a joining
 * device needs within ~200 us, so association requests are retried into
 * silence. Stopping the STA for the window hands the antenna to 802.15.4
 * outright; the hub is off the LAN until resume (the Zigbee tab's pairing
 * countdown already expects this). Safe from any task: both post onto the
 * default event loop. Pause is a no-op in AP/portal mode and when already
 * paused; resume is a no-op when not paused. Resume re-runs the boot
 * pre-scan so the reconnect does not hit the start-then-connect race. */
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_get_ip(char out[16]);
