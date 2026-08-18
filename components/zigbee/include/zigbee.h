/* zigbee.h -- the Zigbee coordinator (M6b spec section 3).
 *
 * The stack task owns esp_zb_stack_main_loop(); everything else reaches it
 * through the signal handler. Bring-up NEVER blocks app_main(): a
 * coordinator that cannot form is a degraded hub and an alert, not a boot
 * failure -- the hub still collects BLE and serves its UI.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Starts the stack task. Returns ESP_OK once the task is created -- NOT
 * once a network exists; formation is asynchronous and reported through
 * zigbee_net_info(). Safe to call when CONFIG_PLANTHUB_ZB_ENABLED is off:
 * it returns ESP_OK and does nothing. */
esp_err_t zigbee_start(void);

/* Current network state for the UI. Returns false when Zigbee is disabled
 * at build time or the stack has not started. */
bool zigbee_net_info(uint8_t *channel, uint16_t *pan_id, bool *formed);

/* Opens the permit-join window for CONFIG_PLANTHUB_ZB_PERMIT_JOIN_S
 * seconds. Returns false when there is no formed network to join. */
bool zigbee_permit_join(void);

/* Seconds left in the permit-join window, 0 when closed. */
uint8_t zigbee_permit_join_remaining(void);
