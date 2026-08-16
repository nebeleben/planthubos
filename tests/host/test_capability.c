#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "capability.h"

int main(void) {
    /* table integrity: ids dense 0..7, unique non-empty names, lookup agrees */
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
        const capability_t *c = capability_get(i);
        assert(c && c->id == i && c->name && c->name[0] && c->unit);
        assert(capability_by_name(c->name) == c);
        for (uint8_t j = 0; j < i; j++)
            assert(strcmp(capability_get(j)->name, c->name) != 0);
    }
    assert(capability_get(CAPABILITY_COUNT) == NULL);
    assert(capability_by_name("nope.nope") == NULL);

    /* frozen ids (M1 bytecode compatibility) */
    assert(strcmp(capability_get(0)->name, "soil.moisture") == 0);
    assert(strcmp(capability_get(1)->name, "air.temperature") == 0);
    assert(strcmp(capability_get(2)->name, "light.illuminance") == 0);
    assert(strcmp(capability_get(3)->name, "soil.conductivity") == 0);
    assert(strcmp(capability_get(4)->name, "battery.level") == 0);

    /* scale round-trips at range extremes */
    assert(capability_encode(CAP_SOIL_MOISTURE, 0.0f) == 0);
    assert(capability_encode(CAP_SOIL_MOISTURE, 100.0f) == 100);
    assert(fabsf(capability_decode(CAP_AIR_TEMPERATURE,
           capability_encode(CAP_AIR_TEMPERATURE, -12.3f)) + 12.3f) < 0.05f);
    assert(fabsf(capability_decode(CAP_AIR_TEMPERATURE,
           capability_encode(CAP_AIR_TEMPERATURE, 45.6f)) - 45.6f) < 0.05f);
    /* lux /16 must cover a bright day without overflowing int16 */
    int16_t lux_hi = capability_encode(CAP_LIGHT_ILLUMINANCE, 100000.0f);
    assert(lux_hi != CAP_VALUE_NONE && lux_hi > 0);
    assert(fabsf(capability_decode(CAP_LIGHT_ILLUMINANCE, lux_hi) - 100000.0f) < 20.0f);
    /* pressure offset form */
    assert(fabsf(capability_decode(CAP_AIR_PRESSURE,
           capability_encode(CAP_AIR_PRESSURE, 1013.2f)) - 1013.2f) < 0.05f);

    /* out-of-range -> CAP_VALUE_NONE, not a wrapped value */
    assert(capability_encode(CAP_LIGHT_ILLUMINANCE, 9000000.0f) == CAP_VALUE_NONE);
    assert(capability_encode(CAP_AIR_PRESSURE, 100.0f) == CAP_VALUE_NONE);
    /* sentinel decodes to NAN */
    assert(isnan(capability_decode(CAP_SOIL_MOISTURE, CAP_VALUE_NONE)));

    printf("test_capability: all passed\n");
    return 0;
}
