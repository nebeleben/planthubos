#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  plant_id;
    char     name[33];          /* plant name, "" = unnamed -- emitted as a
                                  * string field (not a tag) only when set */
    char     sensor_mac12[13];  /* assigned probe's mac, 12 uppercase hex +
                                  * NUL, "" = no probe -- emitted as a string
                                  * field only when set */
    bool     has_temp, has_moisture, has_lux, has_conductivity, has_battery;
    float    temp_c;
    uint8_t  moisture_pct;
    uint32_t lux;
    uint16_t conductivity;
    uint8_t  battery_pct;
    int64_t  epoch_s;         /* absolute timestamp of the reading */
} lp_point_t;

/* Appends one line-protocol line (terminated with '\n') to buf+*off:
 *   plant,plant=<plant_id> temp=...,moisture=...i,lux=...i,conductivity=...i,
 *     battery=...i,name="<esc>",sensor="<esc>" <epoch_s>
 * (only fields whose has_* flag/non-empty string is set are emitted). name
 * and sensor_mac12 are line-protocol STRING FIELDS -- '"' and '\' are
 * escaped -- not tags.
 * Returns true on success; false (buf untouched, *off unchanged) if it
 * would not fit or no has_* flag is set. */
bool lineproto_append(char *buf, size_t cap, size_t *off, const lp_point_t *p);
