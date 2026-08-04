#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "mqtt_json.h"

int main(void)
{
    char t[128], j[640];

    assert(mqtt_topic_state(t, sizeof t, "PlantHub-7814", "80EACA892563"));
    assert(strcmp(t, "planthub/PlantHub-7814/80EACA892563/state") == 0);
    assert(mqtt_topic_avail(t, sizeof t, "PlantHub-7814"));
    assert(strcmp(t, "planthub/PlantHub-7814/status") == 0);
    assert(mqtt_topic_discovery(t, sizeof t, "80EACA892563", "temp"));
    assert(strcmp(t, "homeassistant/sensor/planthub_80EACA892563_temp/config") == 0);
    char small[8];
    assert(!mqtt_topic_state(small, sizeof small, "PlantHub-7814", "80EACA892563"));

    mqtt_state_t st = { .has_temp = 1, .temp_c = 27.3f, .has_moisture = 1,
                        .moisture_pct = 18, .rssi = -88 };
    assert(mqtt_json_state(j, sizeof j, &st));
    assert(strcmp(j, "{\"temp\":27.3,\"moisture\":18,\"rssi\":-88}") == 0);

    mqtt_state_t none = { .rssi = -70 };
    assert(mqtt_json_state(j, sizeof j, &none));
    assert(strcmp(j, "{\"rssi\":-70}") == 0);

    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", "80EACA892563", "Palme", "temp"));
    assert(strstr(j, "\"uniq_id\":\"planthub_80EACA892563_temp\"") != NULL);
    assert(strstr(j, "\"stat_t\":\"planthub/PlantHub-7814/80EACA892563/state\"") != NULL);
    assert(strstr(j, "\"avty_t\":\"planthub/PlantHub-7814/status\"") != NULL);
    assert(strstr(j, "\"dev_cla\":\"temperature\"") != NULL);
    assert(strstr(j, "\"name\":\"Palme temp\"") != NULL);
    assert(strstr(j, "\"via_device\":\"PlantHub-7814\"") != NULL);

    /* no name falls back to mac12; conductivity omits dev_cla */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", "80EACA892563", "", "conductivity"));
    assert(strstr(j, "\"name\":\"80EACA892563 conductivity\"") != NULL);
    assert(strstr(j, "dev_cla") == NULL);
    assert(strstr(j, "\"unit_of_meas\":\"\\u00b5S/cm\"") != NULL);

    assert(!mqtt_json_discovery(j, sizeof j, "h", "m", "", "bogus"));

    printf("test_mqtt_json: OK\n");
    return 0;
}
