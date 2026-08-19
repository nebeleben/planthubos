#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CAPABILITY_COUNT 9
#define CAP_NONE         0xFF          /* "no capability" marker (column maps) */
#define CAP_VALUE_NONE   INT16_MIN     /* stored sentinel: no value */

/* Frozen ids 0-4 (M1 bytecode references them numerically); 5-7 also frozen
 * (M3/M4 bytecode). CAP_SWITCH_STATE (M5b) is appended at the end, never
 * renumbered: an actuator's on/off state is an ORDINARY capability so
 * history, MQTT, HA and Influx render it with no new code (spec section
 * 1.3). */
enum {
    CAP_SOIL_MOISTURE = 0, CAP_AIR_TEMPERATURE = 1, CAP_LIGHT_ILLUMINANCE = 2,
    CAP_SOIL_CONDUCTIVITY = 3, CAP_BATTERY_LEVEL = 4, CAP_AIR_HUMIDITY = 5,
    CAP_AIR_PRESSURE = 6, CAP_SIGNAL_RSSI = 7, CAP_SWITCH_STATE = 8,
};

typedef struct {
    uint8_t     id;
    const char *name;            /* "soil.moisture" */
    const char *unit;            /* "%", "°C", "lux", "µS/cm", "hPa", "dBm" -- UTF-8
                                   * (spec §1's table), see capability.c's Triage
                                   * item 9 doc comment */
    const char *ha_device_class; /* NULL when HA has none (e.g. conductivity) */
    const char *influx_field;    /* "moisture", "temp", ... (short, stable) */
    float       scale;           /* stored = round((value - offset) * scale) */
    float       offset;
    uint8_t     precision;       /* display decimals */
} capability_t;

const capability_t *capability_get(uint8_t id);          /* NULL if unknown */
const capability_t *capability_by_name(const char *name); /* NULL if unknown */

/* Value <-> storage conversions. capability_encode returns CAP_VALUE_NONE for
 * an out-of-range value (logged by callers, never silently clamped). */
int16_t capability_encode(uint8_t id, float value);
float   capability_decode(uint8_t id, int16_t raw);       /* raw==CAP_VALUE_NONE -> NAN */

/* --- device_id.c --- */
typedef enum { DEV_KIND_BLE = 0, DEV_KIND_ESPNOW = 1, DEV_KIND_ZIGBEE = 2 } device_kind_t;
/* Deliberately a #define, NOT a fourth enumerator in device_kind_t itself
 * (M6b Task 7, correction to the original dispatch table): device_id.c's
 * kind_prefix() and kind_addr_len() both switch over device_kind_t with no
 * `default:` label, exhaustive over exactly the three real kinds above,
 * and every host test that links capability.c builds with
 * -Wall -Wextra -Werror. A fourth enumerator would make both switches
 * non-exhaustive and turn -Wswitch into a hard build break there. A plain
 * macro gives actor.c a table size without touching the enum those
 * switches are exhaustive over. */
#define DEV_KIND_COUNT 3
typedef struct { uint8_t kind; uint8_t addr[8]; } device_id_t;   /* 9 bytes */

device_id_t device_id_from_mac(device_kind_t kind, const uint8_t mac[6]);  /* pads 6->8 */
bool        device_id_equal(const device_id_t *a, const device_id_t *b);
/* Text form per spec section 2. buf >= 24 bytes. Returns buf. */
char       *device_id_format(const device_id_t *id, char *buf, size_t buflen);
/* Accepts canonical form and (for ble/espnow) colon-separated MAC. */
bool        device_id_parse(const char *s, device_id_t *out);
