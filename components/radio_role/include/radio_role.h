#pragma once
/* radio_role.h -- which ONE sensor radio this node runs: BLE or Zigbee
 * (or neither). Measured (radio-architecture findings doc): BLE scanning
 * starves the 802.15.4 coordinator on a shared antenna, so a node picks
 * one. Persisted in NVS namespace "planthub", key "radio_role", as the
 * string from radio_role_str(); applied by reboot (the BT and 802.15.4
 * controllers cannot be re-inited live). Unset, or an unknown string,
 * means the Kconfig default and radio_role_is_set() == false, which is
 * what the webui's onboarding step keys on. */
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
 * updated before the NVS write, same convention as swarm_store_set_role(). */
esp_err_t    radio_role_set(radio_role_t role);
/* The CONFIG_PLANTHUB_DEFAULT_RADIO_ROLE choice. */
radio_role_t radio_role_default(void);
