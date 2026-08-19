/* Host test for zb_map.c (M6b spec section 6). Pure table + unit
 * conversion, no ESP-IDF, so plain `cc` exercises it directly. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "zb_map.h"
#include "capability.h"
#include "action.h"

static int close_to(float a, float b) { return fabsf(a - b) < 0.01f; }

int main(void) {
    /* --- cluster -> capability --- */
    assert(zb_map_cluster_to_cap(0x0402) == CAP_AIR_TEMPERATURE);
    assert(zb_map_cluster_to_cap(0x0405) == CAP_AIR_HUMIDITY);
    assert(zb_map_cluster_to_cap(0x0403) == CAP_AIR_PRESSURE);
    assert(zb_map_cluster_to_cap(0x0400) == CAP_LIGHT_ILLUMINANCE);
    assert(zb_map_cluster_to_cap(0x0408) == CAP_SOIL_MOISTURE);
    assert(zb_map_cluster_to_cap(0x0001) == CAP_BATTERY_LEVEL);
    assert(zb_map_cluster_to_cap(0x0006) == CAP_SWITCH_STATE);
    /* An unmapped cluster is not an error -- it is M6c's input. */
    assert(zb_map_cluster_to_cap(0xEF00) == ZB_MAP_NONE);
    assert(zb_map_cluster_to_cap(0x0000) == ZB_MAP_NONE);

    /* --- cluster -> actions --- */
    uint8_t acts[4];
    assert(zb_map_cluster_to_actions(0x0006, acts, 4) == 2);
    assert(acts[0] == ACT_SWITCH_ON && acts[1] == ACT_SWITCH_OFF);
    assert(zb_map_cluster_to_actions(0x0402, acts, 4) == 0);   /* sensors have none */
    assert(zb_map_cluster_to_actions(0x0006, acts, 1) == 1);   /* respects max */

    /* --- reportable attribute ids --- */
    assert(zb_map_report_attr(0x0402) == 0x0000);   /* MeasuredValue */
    assert(zb_map_report_attr(0x0006) == 0x0000);   /* OnOff */
    assert(zb_map_report_attr(0x0001) == 0x0021);   /* BatteryPercentageRemaining */
    assert(zb_map_report_attr(0xEF00) == 0xFFFF);

    /* --- raw ZCL -> the capability's own unit --- */
    float v;
    /* Temperature: int16 in 0.01 C. */
    assert(zb_map_zcl_to_value(0x0402, 2310, &v) && close_to(v, 23.10f));
    assert(zb_map_zcl_to_value(0x0402, -500, &v) && close_to(v, -5.00f));
    /* Humidity: uint16 in 0.01 %. */
    assert(zb_map_zcl_to_value(0x0405, 4550, &v) && close_to(v, 45.50f));
    /* Pressure: ZCL MeasuredValue is 10 x kPa, which is numerically hPa. */
    assert(zb_map_zcl_to_value(0x0403, 1013, &v) && close_to(v, 1013.0f));
    /* Battery: uint8 in 0.5 %. */
    assert(zb_map_zcl_to_value(0x0001, 190, &v) && close_to(v, 95.0f));
    /* Soil moisture: uint16 in 0.01 %. */
    assert(zb_map_zcl_to_value(0x0408, 3300, &v) && close_to(v, 33.0f));
    /* On/Off: boolean straight through. */
    assert(zb_map_zcl_to_value(0x0006, 1, &v) && close_to(v, 1.0f));
    assert(zb_map_zcl_to_value(0x0006, 0, &v) && close_to(v, 0.0f));
    /* Illuminance is LOGARITHMIC: MeasuredValue = 10000*log10(lux) + 1. */
    assert(zb_map_zcl_to_value(0x0400, 30001, &v) && close_to(v, 1000.0f));
    assert(zb_map_zcl_to_value(0x0400, 1, &v) && close_to(v, 1.0f));
    /* ZCL's two illuminance sentinels must NOT become readings. */
    assert(!zb_map_zcl_to_value(0x0400, 0, &v));        /* too dark to measure */
    assert(!zb_map_zcl_to_value(0x0400, 0xFFFF, &v));   /* invalid */

    /* Every cluster has ZCL sentinels that must NOT become fabricated
     * readings. A gap in a plant's history is better than a fabricated
     * value that looks like data. Test both the sentinel and a legitimate
     * value near each to ensure the guard is not over-broad. */
    /* Temperature: 0x8000 is invalid in both sign-extended and unsigned form. */
    assert(!zb_map_zcl_to_value(0x0402, 0x8000, &v));   /* sentinel 0x8000 */
    assert(!zb_map_zcl_to_value(0x0402, -32768, &v));   /* sentinel as signed */
    assert(zb_map_zcl_to_value(0x0402, 2500, &v) && close_to(v, 25.0f)); /* valid */
    /* Humidity: 0xFFFF is invalid. */
    assert(!zb_map_zcl_to_value(0x0405, 0xFFFF, &v));   /* sentinel 0xFFFF */
    assert(zb_map_zcl_to_value(0x0405, 9998, &v) && close_to(v, 99.98f)); /* valid */
    /* Soil Moisture: 0xFFFF is invalid. */
    assert(!zb_map_zcl_to_value(0x0408, 0xFFFF, &v));   /* sentinel 0xFFFF */
    assert(zb_map_zcl_to_value(0x0408, 5000, &v) && close_to(v, 50.0f)); /* valid */
    /* Pressure: 0x8000 is invalid in both sign-extended and unsigned form. */
    assert(!zb_map_zcl_to_value(0x0403, 0x8000, &v));   /* sentinel 0x8000 */
    assert(!zb_map_zcl_to_value(0x0403, -32768, &v));   /* sentinel as signed */
    assert(zb_map_zcl_to_value(0x0403, 950, &v) && close_to(v, 950.0f)); /* valid */
    /* Battery: 0xFF is unknown. */
    assert(!zb_map_zcl_to_value(0x0001, 0xFF, &v));     /* sentinel 0xFF */
    assert(zb_map_zcl_to_value(0x0001, 200, &v) && close_to(v, 100.0f)); /* valid */

    /* An unmapped cluster yields no value. */
    assert(!zb_map_zcl_to_value(0xEF00, 1234, &v));

    printf("test_zb_map: OK\n");
    return 0;
}
