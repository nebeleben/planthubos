#include "zb_map.h"
#include "capability.h"
#include "action.h"
#include <math.h>

#define CL_POWER_CONFIG  0x0001
#define CL_ON_OFF        0x0006
#define CL_ILLUMINANCE   0x0400
#define CL_TEMPERATURE   0x0402
#define CL_PRESSURE      0x0403
#define CL_HUMIDITY      0x0405
#define CL_SOIL_MOISTURE 0x0408

uint8_t zb_map_cluster_to_cap(uint16_t cluster) {
    switch (cluster) {
        case CL_TEMPERATURE:   return CAP_AIR_TEMPERATURE;
        case CL_HUMIDITY:      return CAP_AIR_HUMIDITY;
        case CL_PRESSURE:      return CAP_AIR_PRESSURE;
        case CL_ILLUMINANCE:   return CAP_LIGHT_ILLUMINANCE;
        case CL_SOIL_MOISTURE: return CAP_SOIL_MOISTURE;
        case CL_POWER_CONFIG:  return CAP_BATTERY_LEVEL;
        case CL_ON_OFF:        return CAP_SWITCH_STATE;
        default:               return ZB_MAP_NONE;
    }
}

int zb_map_cluster_to_actions(uint16_t cluster, uint8_t *out, int max) {
    if (!out || max <= 0 || cluster != CL_ON_OFF)
        return 0;
    static const uint8_t on_off[2] = { ACT_SWITCH_ON, ACT_SWITCH_OFF };
    int n = (max < 2) ? max : 2;
    for (int i = 0; i < n; i++)
        out[i] = on_off[i];
    return n;
}

uint16_t zb_map_report_attr(uint16_t cluster) {
    switch (cluster) {
        /* MeasuredValue on every measurement cluster, OnOff on 0x0006 --
         * all of them attribute 0x0000. */
        case CL_TEMPERATURE:
        case CL_HUMIDITY:
        case CL_PRESSURE:
        case CL_ILLUMINANCE:
        case CL_SOIL_MOISTURE:
        case CL_ON_OFF:        return 0x0000;
        /* BatteryPercentageRemaining, not BatteryVoltage: percentage is
         * what CAP_BATTERY_LEVEL stores and it needs no chemistry curve. */
        case CL_POWER_CONFIG:  return 0x0021;
        default:               return ZB_MAP_NO_ATTR;
    }
}

bool zb_map_zcl_to_value(uint16_t cluster, int32_t raw, float *out) {
    if (!out)
        return false;
    switch (cluster) {
        case CL_TEMPERATURE:                  /* int16, 0.01 C */
            /* ZCL sentinel: 0x8000 (invalid). Reject both sign-extended
             * (-32768) and unsigned (32768) spellings. A fabricated reading
             * in a plant's history is worse than a gap: a gap is visibly
             * missing while a -327.68 °C value looks like data. */
            if (raw == 0x8000 || raw == -32768)
                return false;
            *out = (float)raw / 100.0f;
            return true;
        case CL_HUMIDITY:                     /* uint16, 0.01 % */
            /* ZCL sentinel: 0xFFFF (invalid). A fabricated 655.35 % reading
             * in a plant's history is worse than a gap. */
            if (raw == 0xFFFF)
                return false;
            *out = (float)raw / 100.0f;
            return true;
        case CL_SOIL_MOISTURE:                /* uint16, 0.01 % */
            /* ZCL sentinel: 0xFFFF (invalid). A fabricated 655.35 % reading
             * in a plant's history is worse than a gap. */
            if (raw == 0xFFFF)
                return false;
            *out = (float)raw / 100.0f;
            return true;
        case CL_PRESSURE:
            /* ZCL MeasuredValue is 10 x pressure-in-kPa, and 1 kPa is
             * 10 hPa, so the number IS hPa -- 101.325 kPa reports as 1013,
             * which is 1013 hPa. CAP_AIR_PRESSURE's unit is hPa.
             * ZCL sentinel: 0x8000 (invalid). Reject both sign-extended
             * (-32768) and unsigned (32768) spellings. A fabricated reading
             * in a plant's history is worse than a gap. */
            if (raw == 0x8000 || raw == -32768)
                return false;
            *out = (float)raw;
            return true;
        case CL_POWER_CONFIG:                 /* uint8, 0.5 % units */
            /* ZCL sentinel: 0xFF (unknown). A fabricated 127.5 % reading
             * in a plant's history is worse than a gap. */
            if (raw == 0xFF)
                return false;
            *out = (float)raw / 2.0f;
            return true;
        case CL_ON_OFF:
            *out = raw ? 1.0f : 0.0f;
            return true;
        case CL_ILLUMINANCE:
            /* MeasuredValue = 10000 * log10(lux) + 1, so this is the only
             * non-linear conversion here. ZCL reserves two values that are
             * NOT measurements: 0 means "too dark to measure" and 0xFFFF
             * means "invalid". Storing either as a lux reading would put a
             * fabricated number in the user's history. */
            if (raw == 0 || raw == 0xFFFF)
                return false;
            *out = powf(10.0f, ((float)raw - 1.0f) / 10000.0f);
            return true;
        default:
            return false;
    }
}
