#include "capability.h"
#include <math.h>
#include <string.h>

/*
 * Single source of truth for capability metadata. Ids 0-4 are frozen (M1
 * bytecode compatibility) - never renumber them. influx_field values reuse
 * the V1 spellings (moisture, temp, lux, conductivity, battery) so existing
 * dashboards keep working unchanged; the new capabilities (humidity,
 * pressure, rssi) follow the same short/stable-name convention.
 */
static const capability_t CAP_TABLE[CAPABILITY_COUNT] = {
    [CAP_SOIL_MOISTURE] = {
        .id = CAP_SOIL_MOISTURE, .name = "soil.moisture", .unit = "%",
        .ha_device_class = "moisture", .influx_field = "moisture",
        .scale = 1.0f, .offset = 0.0f, .precision = 0,
    },
    [CAP_AIR_TEMPERATURE] = {
        .id = CAP_AIR_TEMPERATURE, .name = "air.temperature", .unit = "C",
        .ha_device_class = "temperature", .influx_field = "temp",
        .scale = 10.0f, .offset = 0.0f, .precision = 1,
    },
    [CAP_LIGHT_ILLUMINANCE] = {
        .id = CAP_LIGHT_ILLUMINANCE, .name = "light.illuminance", .unit = "lux",
        .ha_device_class = "illuminance", .influx_field = "lux",
        .scale = 1.0f / 16.0f, .offset = 0.0f, .precision = 0,
    },
    [CAP_SOIL_CONDUCTIVITY] = {
        .id = CAP_SOIL_CONDUCTIVITY, .name = "soil.conductivity", .unit = "uS/cm",
        .ha_device_class = NULL, .influx_field = "conductivity",
        .scale = 1.0f, .offset = 0.0f, .precision = 0,
    },
    [CAP_BATTERY_LEVEL] = {
        .id = CAP_BATTERY_LEVEL, .name = "battery.level", .unit = "%",
        .ha_device_class = "battery", .influx_field = "battery",
        .scale = 1.0f, .offset = 0.0f, .precision = 0,
    },
    [CAP_AIR_HUMIDITY] = {
        .id = CAP_AIR_HUMIDITY, .name = "air.humidity", .unit = "%",
        .ha_device_class = "humidity", .influx_field = "humidity",
        .scale = 10.0f, .offset = 0.0f, .precision = 1,
    },
    [CAP_AIR_PRESSURE] = {
        .id = CAP_AIR_PRESSURE, .name = "air.pressure", .unit = "hPa",
        .ha_device_class = "atmospheric_pressure", .influx_field = "pressure",
        .scale = 10.0f, .offset = 900.0f, .precision = 1,
    },
    [CAP_SIGNAL_RSSI] = {
        .id = CAP_SIGNAL_RSSI, .name = "signal.rssi", .unit = "dBm",
        .ha_device_class = "signal_strength", .influx_field = "rssi",
        .scale = 1.0f, .offset = 0.0f, .precision = 0,
    },
};

const capability_t *capability_get(uint8_t id) {
    if (id >= CAPABILITY_COUNT)
        return NULL;
    return &CAP_TABLE[id];
}

const capability_t *capability_by_name(const char *name) {
    if (!name)
        return NULL;
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++)
        if (strcmp(CAP_TABLE[i].name, name) == 0)
            return &CAP_TABLE[i];
    return NULL;
}

int16_t capability_encode(uint8_t id, float value) {
    const capability_t *c = capability_get(id);
    if (!c)
        return CAP_VALUE_NONE;

    double scaled = ((double)value - (double)c->offset) * (double)c->scale;
    double rounded = round(scaled);

    /*
     * Offset-form capabilities (currently only air.pressure) encode a
     * physical quantity whose valid domain starts at the offset - the
     * scale/offset pair is chosen so the entire realistic range maps to
     * non-negative storage (see spec Sec 1: "pressure covers 900-4176 hPa").
     * A negative result therefore means the input fell below that floor and
     * must be rejected rather than silently accepted as a huge negative
     * pressure. Zero-offset capabilities (temperature, rssi, ...) are
     * allowed to go negative, using the full int16 range.
     */
    double lo = (c->offset != 0.0f) ? 0.0 : (double)(INT16_MIN + 1);
    double hi = (double)INT16_MAX;
    if (rounded < lo || rounded > hi)
        return CAP_VALUE_NONE;

    return (int16_t)rounded;
}

float capability_decode(uint8_t id, int16_t raw) {
    const capability_t *c = capability_get(id);
    if (!c || raw == CAP_VALUE_NONE)
        return NAN;
    return (float)raw / c->scale + c->offset;
}
