#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "mqtt_json.h"

int main(void)
{
    char t[128], j[640];

    assert(mqtt_topic_state(t, sizeof t, "PlantHub-7814", 7));
    assert(strcmp(t, "planthub/PlantHub-7814/plant/7/state") == 0);
    assert(mqtt_topic_avail(t, sizeof t, "PlantHub-7814"));
    assert(strcmp(t, "planthub/PlantHub-7814/status") == 0);
    assert(mqtt_topic_discovery(t, sizeof t, 7, "temp"));
    assert(strcmp(t, "homeassistant/sensor/planthub_plant_7_temp/config") == 0);
    char small[8];
    assert(!mqtt_topic_state(small, sizeof small, "PlantHub-7814", 7));

    mqtt_state_t st = { .has_temp = 1, .temp_c = 27.3f, .has_moisture = 1,
                        .moisture_pct = 18, .rssi = -88 };
    assert(mqtt_json_state(j, sizeof j, &st));
    assert(strcmp(j, "{\"temp\":27.3,\"moisture\":18,\"rssi\":-88}") == 0);

    mqtt_state_t none = { .rssi = -70 };
    assert(mqtt_json_state(j, sizeof j, &none));
    assert(strcmp(j, "{\"rssi\":-70}") == 0);

    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "Palme", "temp"));
    assert(strstr(j, "\"uniq_id\":\"planthub_plant_7_temp\"") != NULL);
    assert(strstr(j, "\"stat_t\":\"planthub/PlantHub-7814/plant/7/state\"") != NULL);
    assert(strstr(j, "\"avty_t\":\"planthub/PlantHub-7814/status\"") != NULL);
    assert(strstr(j, "\"dev_cla\":\"temperature\"") != NULL);
    assert(strstr(j, "\"name\":\"Palme temp\"") != NULL);
    assert(strstr(j, "\"via_device\":\"PlantHub-7814\"") != NULL);
    assert(strstr(j, "\"unit_of_meas\":\"\\u00b0C\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"Palme\",") != NULL);

    /* no name falls back to "Plant <id>", built inside the discovery
     * builder itself; conductivity omits dev_cla */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "", "conductivity"));
    assert(strstr(j, "\"name\":\"Plant 7 conductivity\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"Plant 7\",") != NULL);
    assert(strstr(j, "dev_cla") == NULL);
    assert(strstr(j, "\"unit_of_meas\":\"\\u00b5S/cm\"") != NULL);

    assert(!mqtt_json_discovery(j, sizeof j, "h", 1, "", "bogus"));

    /* a name containing a double quote must not break the JSON: both the
     * "name" field and dev.name escape it. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "a\"b", "temp"));
    assert(strstr(j, "\"name\":\"a\\\"b temp\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"a\\\"b\",") != NULL);

    /* a name containing a backslash and a control char is escaped too. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "a\\b\tc", "temp"));
    assert(strstr(j, "\"name\":\"a\\\\b\\u0009c temp\"") != NULL);

    printf("test_mqtt_json: OK\n");
    return 0;
}
