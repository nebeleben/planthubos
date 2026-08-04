#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  mac[6];
    char     name[33];        /* "" = no name; emitted as a tag only when set */
    bool     has_temp, has_moisture, has_lux, has_conductivity, has_battery;
    float    temp_c;
    uint8_t  moisture_pct;
    uint32_t lux;
    uint16_t conductivity;
    uint8_t  battery_pct;
    int64_t  epoch_s;         /* absolute timestamp of the reading */
} lp_point_t;

/* Appends one line-protocol line (terminated with '\n') to buf+*off.
 * Returns true on success; false (buf untouched, *off unchanged) if it
 * would not fit or no has_* flag is set. */
bool lineproto_append(char *buf, size_t cap, size_t *off, const lp_point_t *p);
