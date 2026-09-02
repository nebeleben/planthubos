#pragma once
/* radio_role.h -- which ONE sensor radio this node runs: BLE or Zigbee
 * (or neither). Measured (radio-architecture findings doc): BLE scanning
 * starves the 802.15.4 coordinator on a shared antenna, so a node picks
 * one. Persisted in NVS namespace "planthub", key "radio_role", as the
 * string from radio_role_str(); applied by reboot (the BT and 802.15.4
 * controllers cannot be re-inited live). Unset, or an unknown string,
 * means the Kconfig default and radio_role_is_set() == false, which is
 * what the webui's onboarding step keys on.
 *
 * s_role/s_set (the RAM cache) are NOT mutex-protected, unlike
 * swarm_store's cache. That is safe only because of two non-overlapping
 * access windows: (1) radio_role_init() runs once, early in app_main, in
 * the single main task, strictly before webserver_start(); (2) after that,
 * every reader (radio_role_get()/radio_role_is_set()) and the only writer
 * (radio_role_set()) run from httpd request handlers, and esp_http_server
 * serializes all handlers on one worker task -- so there is never more
 * than one task touching the cache at a time. If a second writer task
 * (e.g. a console command, a future OTA/config path) is ever added, add a
 * FreeRTOS mutex around get/is_set/set at that point. */
#include "esp_err.h"
#include "radio_role_str.h"

/* Load NVS into the RAM cache. Call once, early in app_main, BEFORE
 * webserver_start() (the status handler reads the cache from the httpd
 * task). Fresh NVS is not an error. */
esp_err_t    radio_role_init(void);
/* Effective role: the stored one if set, else radio_role_default(). */
radio_role_t radio_role_get(void);
/* true when NVS held a valid role at init or radio_role_set() succeeded since. */
bool         radio_role_is_set(void);
/* Persist. ESP_ERR_INVALID_ARG on an out-of-range value. The RAM cache is
 * updated before the NVS write; s_role is written before s_set is set to
 * true (both here and in radio_role_init()) so a reader can never observe
 * is_set() == true paired with a stale role. See the no-mutex note above
 * for why that ordering is sufficient instead of a lock. */
esp_err_t    radio_role_set(radio_role_t role);
/* The CONFIG_PLANTHUB_DEFAULT_RADIO_ROLE choice. */
radio_role_t radio_role_default(void);
