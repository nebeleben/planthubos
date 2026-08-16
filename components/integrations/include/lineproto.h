#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capability.h"

/* Capability-driven Influx line-protocol builder (M2 Task 7, spec Sec.6):
 * one field per present capability, keyed by influx_field (the capability
 * table's short/stable spelling -- moisture, temp, lux, conductivity,
 * battery keep their V1 names so existing dashboards keep working;
 * humidity/pressure/rssi are new). value[i] is the DECODED (real-unit)
 * value, same contract as mqtt_json.h's mqtt_state_t. A capability whose
 * table precision is 0 is written as an Influx INTEGER field ("...i"
 * suffix, rounded); a non-zero precision is written as a FLOAT field with
 * that many decimals -- the same split V1 hardcoded (moisture/lux/
 * conductivity/battery integer, temp float), generalised. */
typedef struct {
    bool  present[CAPABILITY_COUNT];
    float value[CAPABILITY_COUNT];
} lp_fields_t;

typedef struct {
    uint8_t     plant_id;
    lp_fields_t fields;
    int64_t     epoch_s;
} lp_plant_point_t;

/* Appends one line-protocol line (terminated with '\n') to buf+*off:
 *   plant,plant=<plant_id> <influx_field>=...,... <epoch_s>
 * Returns true on success; false (buf untouched, *off unchanged) if it
 * would not fit or no capability is present. */
bool lineproto_append_plant(char *buf, size_t cap, size_t *off, const lp_plant_point_t *p);

typedef struct {
    char        dev_id_str[24];  /* capability.h's device_id_format() output */
    lp_fields_t fields;
    int64_t     epoch_s;
} lp_device_point_t;

/* Same shape, measurement "device", tag device=<dev_id_str>:
 *   device,device=<dev_id_str> <influx_field>=...,... <epoch_s>
 * Same fit/no-fields contract as lineproto_append_plant() above. */
bool lineproto_append_device(char *buf, size_t cap, size_t *off, const lp_device_point_t *p);
