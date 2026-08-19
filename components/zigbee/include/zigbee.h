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

/* ---------------------------------------------------------------------
 * Task 8: the Zigbee command engine (zb_cmd.c). Split across this header's
 * two owners the same way the rest of this file is: zigbee.c owns the
 * store and the coordinator's own endpoint, so it supplies the two lookups
 * below; zb_cmd.c owns dispatch and correlating a Default Response back to
 * whichever command it sent, so it supplies the other two. Neither
 * direction needs the Zigbee SDK's own types in this header -- every
 * signature below is plain stdint, so a consumer that never touches
 * esp_zigbee_core.h (webserver code, for one) is unaffected by this
 * component's growth.
 * --------------------------------------------------------------------- */

/* The joined-device store's short_addr/endpoint for a device already known
 * by EUI-64 -- zb_cmd.c's one need from the store zigbee.c owns. Copies
 * both out under the store's own lock and returns immediately; never holds
 * that lock across the SDK call the caller makes next with these values
 * (M6b spec section 3's own constraint on this store). False (out params
 * untouched) when eui64 is not in the store, or Zigbee is disabled/not
 * started. */
bool zigbee_store_lookup(const uint8_t eui64[8], uint16_t *short_addr, uint8_t *endpoint);

/* The coordinator's own ZCL source endpoint (zigbee.c's private
 * ZB_ENDPOINT) -- exposed rather than duplicated as a second magic-number
 * 1 in zb_cmd.c that could silently drift from the real one. */
uint8_t zigbee_coordinator_endpoint(void);

/* Starts the Zigbee command engine: registers the DEV_KIND_ZIGBEE actor
 * dispatch hook (actor_set_dispatch_hook(), M6b Task 7) so a dispatched
 * ACT_SWITCH_ON/ACT_SWITCH_OFF command reaches zb_cmd.c. Call once, from
 * zigbee_start(), after the store has loaded -- a dispatched command
 * resolves its device through the same store zigbee_store_lookup() reads.
 * A safe no-op when Zigbee is disabled at build time. */
void zb_cmd_start(void);

/* Forwards one Default Response's outcome from zigbee.c's single ZCL core
 * action handler (the SDK allows exactly one, network-wide) to zb_cmd.c,
 * which owns correlating it against whichever commands it currently has
 * outstanding by ZCL transaction sequence number (tsn). Plain fields
 * rather than the SDK's esp_zb_zcl_cmd_default_resp_message_t, for the
 * same reason as this section's other two signatures. A response that
 * matches no outstanding command (already timed out, or answering a
 * command this engine never sent) is dropped, not alerted -- see
 * zb_cmd.c's zb_cmd_on_default_resp(). A safe no-op when Zigbee is
 * disabled at build time. */
void zb_cmd_on_default_resp(uint8_t tsn, uint16_t cluster, uint8_t resp_to_cmd,
                             uint8_t status_code);
