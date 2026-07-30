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
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_get_ip(char out[16]);
