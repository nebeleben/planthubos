#pragma once
/* radio_role_str.h -- the pure (no-IDF) half of the radio role: the enum
 * and its wire strings. Kept separate from radio_role.h so the host test
 * can compile it with plain cc. The strings are the NVS value, the JSON
 * value and the webui value; they never differ. */
#include <stdbool.h>

typedef enum {
    RADIO_ROLE_WIFI_ONLY = 0,   /* neither BLE nor Zigbee */
    RADIO_ROLE_BLE       = 1,   /* NimBLE collector (the V1 hub) */
    RADIO_ROLE_ZIGBEE    = 2,   /* 802.15.4 coordinator */
} radio_role_t;

/* "wifi_only" | "ble" | "zigbee". Out-of-range -> "wifi_only" (never NULL). */
const char *radio_role_str(radio_role_t role);

/* Strict, case-sensitive, exact match. false (and *out untouched) on
 * anything else including NULL and "". */
bool radio_role_parse(const char *s, radio_role_t *out);
