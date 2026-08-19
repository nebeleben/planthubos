/* zigbee.h -- the Zigbee coordinator (M6b spec section 3).
 *
 * The stack task owns esp_zb_stack_main_loop(); everything else reaches it
 * through the signal handler. Bring-up NEVER blocks app_main(): a
 * coordinator that cannot form is a degraded hub and an alert, not a boot
 * failure -- the hub still collects BLE and serves its UI.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "zb_store.h"

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

/* Task 6: the joined-device table, for the UI. Copies up to max entries
 * (interviewed AND joined-but-not-interviewed alike -- see zb_store.h's
 * zb_device_t.interviewed) into out, returns how many. 0 when Zigbee is
 * disabled at build time, the stack has not started, or the table is
 * empty. */
int zigbee_device_list(zb_device_t *out, size_t max);

/* Renames a stored device (its user-facing name only; every other field is
 * untouched) and persists the change. False when eui64 is not in the
 * store, name is NULL, or Zigbee is disabled/not started. */
bool zigbee_device_rename(const uint8_t eui64[8], const char *name);

/* Removes a device from the store, persists the change, and asks the
 * stack to remove it from the network. False when eui64 is not in the
 * store, or Zigbee is disabled/not started.
 *
 * registry.h has no delete: any registry entry (capability readings) and
 * actor-table declaration (if the device was an actuator) this device had
 * survive until the next reboot -- they stop updating and the device
 * disappears from this list right away, but that slot is only reclaimed
 * on restart. Do not attempt to add registry deletion in this milestone. */
bool zigbee_device_remove(const uint8_t eui64[8]);
