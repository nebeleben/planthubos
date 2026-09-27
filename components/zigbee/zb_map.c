#include "zb_map.h"
#include "capability.h"
#include "action.h"
#include <math.h>

#define CL_POWER_CONFIG     0x0001
#define CL_ON_OFF           0x0006
#define CL_MULTISTATE_INPUT 0x0012
#define CL_ILLUMINANCE      0x0400
#define CL_TEMPERATURE      0x0402
#define CL_PRESSURE         0x0403
#define CL_HUMIDITY         0x0405
#define CL_SOIL_MOISTURE    0x0408
#define CL_ELECTRICAL_MEAS  0x0B04

#define AT_BATTERY_VOLTAGE   0x0020  /* Power Config: uint8, 100 mV units */
#define AT_BATTERY_PERCENT   0x0021  /* Power Config: uint8, 0.5 % units  */
#define AT_MULTISTATE_PRESENT 0x0055  /* PresentValue, uint16 */
#define AT_EM_ACTIVE_POWER   0x050B  /* ActivePower, int16, per ACPowerDivisor (TS011F: 0.1 W) */
#define AT_EM_RMS_VOLTAGE    0x0505  /* RMSVoltage, uint16, per ACVoltageDivisor (TS011F: 0.1 V) */
#define AT_EM_RMS_CURRENT    0x0508  /* RMSCurrent, uint16, per ACCurrentDivisor (TS011F: 1 mA) */

uint8_t zb_map_cluster_to_cap(uint16_t cluster) {
    switch (cluster) {
        case CL_TEMPERATURE:      return CAP_AIR_TEMPERATURE;
        case CL_HUMIDITY:         return CAP_AIR_HUMIDITY;
        case CL_PRESSURE:         return CAP_AIR_PRESSURE;
        case CL_ILLUMINANCE:      return CAP_LIGHT_ILLUMINANCE;
        case CL_SOIL_MOISTURE:    return CAP_SOIL_MOISTURE;
        case CL_POWER_CONFIG:     return CAP_BATTERY_LEVEL;
        case CL_ON_OFF:           return CAP_SWITCH_STATE;
        case CL_MULTISTATE_INPUT: return CAP_BUTTON_ACTION;
        default:                  return ZB_MAP_NONE;
    }
}

uint8_t zb_map_attr_to_cap(uint16_t cluster, uint16_t attr) {
    /* Electrical Measurement is the only cluster whose attributes map to
     * DIFFERENT caps, so cap selection here is attr-aware; every other
     * cluster has a single cap and ignores attr. cluster_to_cap(0x0B04) is
     * ZB_MAP_NONE on purpose -- metering is discovered by the bridge's
     * blind-probe (not the single-cap interview), and routed here. */
    if (cluster == CL_ELECTRICAL_MEAS) {
        switch (attr) {
            case AT_EM_ACTIVE_POWER: return CAP_ELECTRIC_POWER;
            case AT_EM_RMS_VOLTAGE:  return CAP_ELECTRIC_VOLTAGE;
            case AT_EM_RMS_CURRENT:  return CAP_ELECTRIC_CURRENT;
            default:                 return ZB_MAP_NONE;
        }
    }
    return zb_map_cluster_to_cap(cluster);
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

int zb_map_onoff_backfill_actions(const uint8_t *caps, uint8_t cap_count, uint8_t *out, int max) {
    if (!out || max <= 0)
        return 0;
    /* An input device -- a button or knob -- non-compliantly advertises the
     * On/Off cluster but is NOT an actuator; zb_interview_finalize and
     * knob_reclassify deliberately strip its switch caps and actions. So a
     * device already carrying button.action or dim.rotate must never have the
     * On/Off actions backfilled from a stray 0x0006 report, or we would
     * re-add exactly what those paths removed. Any other device reporting
     * On/Off (including one that interviewed with no clusters at all, e.g. a
     * Tuya plug) is a genuine actuator and gets switch.on/switch.off. */
    for (uint8_t i = 0; caps && i < cap_count; i++)
        if (caps[i] == CAP_BUTTON_ACTION || caps[i] == CAP_DIM_ROTATE)
            return 0;
    return zb_map_cluster_to_actions(CL_ON_OFF, out, max);
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
        case CL_ON_OFF:           return 0x0000;
        /* BatteryPercentageRemaining, not BatteryVoltage: percentage is
         * what CAP_BATTERY_LEVEL stores and it needs no chemistry curve. */
        case CL_POWER_CONFIG:     return 0x0021;
        /* Multistate Input: PresentValue */
        case CL_MULTISTATE_INPUT: return AT_MULTISTATE_PRESENT;
        default:                  return ZB_MAP_NO_ATTR;
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

bool zb_map_accepts_attr(uint16_t cluster, uint16_t attr) {
    if (cluster == CL_ELECTRICAL_MEAS)
        return attr == AT_EM_ACTIVE_POWER || attr == AT_EM_RMS_VOLTAGE
            || attr == AT_EM_RMS_CURRENT;
    if (cluster == CL_POWER_CONFIG)
        return attr == AT_BATTERY_PERCENT || attr == AT_BATTERY_VOLTAGE;
    uint16_t mapped = zb_map_report_attr(cluster);
    return mapped != ZB_MAP_NO_ATTR && attr == mapped;
}

bool zb_map_zcl_attr_to_value(uint16_t cluster, uint16_t attr, int32_t raw, float *out) {
    if (!out)
        return false;
    if (cluster == CL_POWER_CONFIG) {
        if (attr == AT_BATTERY_PERCENT)
            return zb_map_zcl_to_value(cluster, raw, out);  /* 0.5 % units, 0xFF sentinel */
        if (attr == AT_BATTERY_VOLTAGE) {
            /* ZCL sentinel: 0xFF (unknown voltage). */
            if (raw == 0xFF)
                return false;
            /* BatteryVoltage is uint8 in 100 mV units, so mV = raw * 100.
             * Map a coin cell linearly, 2.5 V -> 0 %, 3.0 V -> 100 %:
             * pct = (mV - 2500) / (3000 - 2500) * 100 = raw * 20 - 500.
             * This is an approximation (no per-chemistry curve), documented
             * as such -- a battery LEVEL derived from voltage, not a
             * fabricated sentinel. Clamp to [0, 100]. */
            float pct = (float)raw * 20.0f - 500.0f;
            if (pct < 0.0f)   pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            *out = pct;
            return true;
        }
        return false;
    }
    if (cluster == CL_ELECTRICAL_MEAS) {
        switch (attr) {
            case AT_EM_ACTIVE_POWER:
                /* ActivePower is int16 (signed; export is negative). ZCL
                 * invalid = 0x8000; reject both spellings. TS011F reports
                 * 0.1 W units -> /10 gives W (CAP_ELECTRIC_POWER's unit). */
                if (raw == 0x8000 || raw == -32768) return false;
                *out = (float)raw / 10.0f;
                return true;
            case AT_EM_RMS_VOLTAGE:
                /* uint16, ZCL invalid = 0xFFFF. TS011F 0.1 V units -> /10 V. */
                if (raw == 0xFFFF) return false;
                *out = (float)raw / 10.0f;
                return true;
            case AT_EM_RMS_CURRENT:
                /* uint16, ZCL invalid = 0xFFFF. TS011F 1 mA units -> /1000 A. */
                if (raw == 0xFFFF) return false;
                *out = (float)raw / 1000.0f;
                return true;
            default:
                return false;
        }
    }
    if (cluster == CL_MULTISTATE_INPUT) {
        if (attr != AT_MULTISTATE_PRESENT) return false;
        *out = (float)raw;   /* raw press code, passed through; no ZCL sentinel here */
        return true;
    }
    if (attr != zb_map_report_attr(cluster))
        return false;
    return zb_map_zcl_to_value(cluster, raw, out);
}
