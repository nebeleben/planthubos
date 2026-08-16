#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "lineproto.h"
#include "capability.h"

int main(void)
{
    char buf[512]; size_t off = 0;

    /* Two-capability plant: field names come from influx_field (moisture,
     * temp -- not the capability's dotted name), integer vs float per the
     * capability table's precision (moisture 0dp -> "i" suffix, temp 1dp ->
     * plain float), in capability-id order. */
    lp_plant_point_t p = { .plant_id = 7, .epoch_s = 1785780419 };
    p.fields.present[CAP_SOIL_MOISTURE] = true;
    p.fields.value[CAP_SOIL_MOISTURE] = 18.0f;
    p.fields.present[CAP_AIR_TEMPERATURE] = true;
    p.fields.value[CAP_AIR_TEMPERATURE] = 27.3f;
    assert(lineproto_append_plant(buf, sizeof buf, &off, &p));
    assert(strcmp(buf, "plant,plant=7 moisture=18i,temp=27.3 1785780419\n") == 0);

    /* A single present capability -> no trailing/leading comma. */
    off = 0;
    lp_plant_point_t q = { .plant_id = 3, .epoch_s = 100 };
    q.fields.present[CAP_LIGHT_ILLUMINANCE] = true;
    q.fields.value[CAP_LIGHT_ILLUMINANCE] = 42.0f;
    assert(lineproto_append_plant(buf, sizeof buf, &off, &q));
    assert(strcmp(buf, "plant,plant=3 lux=42i 100\n") == 0);

    /* appends accumulate */
    assert(lineproto_append_plant(buf, sizeof buf, &off, &q));
    assert(off == 2 * strlen("plant,plant=3 lux=42i 100\n"));

    /* nothing present -> rejected, off unchanged */
    off = 0;
    lp_plant_point_t r = { .plant_id = 3, .epoch_s = 100 };
    assert(!lineproto_append_plant(buf, sizeof buf, &off, &r));
    assert(off == 0);

    /* doesn't fit -> rejected, off unchanged */
    char tiny[8]; off = 0;
    assert(!lineproto_append_plant(tiny, sizeof tiny, &off, &q));
    assert(off == 0);

    /* device measurement: tag is device=<id-string>, same field rules. Three
     * present capabilities exercises the ",", join and mixed int/float
     * fields together, and confirms field order follows capability id, not
     * present[] insertion order. */
    off = 0;
    lp_device_point_t d = { .dev_id_str = "ble:A4C138001122", .epoch_s = 1785780500 };
    d.fields.present[CAP_BATTERY_LEVEL] = true;
    d.fields.value[CAP_BATTERY_LEVEL] = 64.0f;
    d.fields.present[CAP_SOIL_CONDUCTIVITY] = true;
    d.fields.value[CAP_SOIL_CONDUCTIVITY] = 118.0f;
    d.fields.present[CAP_AIR_HUMIDITY] = true;
    d.fields.value[CAP_AIR_HUMIDITY] = 55.4f;
    assert(lineproto_append_device(buf, sizeof buf, &off, &d));
    assert(strcmp(buf, "device,device=ble:A4C138001122 "
                  "conductivity=118i,battery=64i,humidity=55.4 1785780500\n") == 0);

    /* a device point with nothing present is rejected too */
    off = 0;
    lp_device_point_t e = { .dev_id_str = "zb:00124B00AABBCCDD", .epoch_s = 1 };
    assert(!lineproto_append_device(buf, sizeof buf, &off, &e));
    assert(off == 0);

    /* rssi (a signed, negative-valued capability) round-trips as a signed
     * integer field, not e.g. wrapping unsigned. */
    off = 0;
    lp_plant_point_t s = { .plant_id = 9, .epoch_s = 5 };
    s.fields.present[CAP_SIGNAL_RSSI] = true;
    s.fields.value[CAP_SIGNAL_RSSI] = -70.0f;
    assert(lineproto_append_plant(buf, sizeof buf, &off, &s));
    assert(strcmp(buf, "plant,plant=9 rssi=-70i 5\n") == 0);

    printf("test_lineproto: OK\n");
    return 0;
}
