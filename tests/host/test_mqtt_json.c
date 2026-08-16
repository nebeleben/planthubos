#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "mqtt_json.h"
#include "capability.h"

int main(void)
{
    char t[128], j[1024];

    assert(mqtt_topic_state(t, sizeof t, "PlantHub-7814", 7));
    assert(strcmp(t, "planthub/PlantHub-7814/plant/7/state") == 0);
    assert(mqtt_topic_device_state(t, sizeof t, "PlantHub-7814", "ble:A4C138001122"));
    assert(strcmp(t, "planthub/PlantHub-7814/device/ble:A4C138001122/state") == 0);
    assert(mqtt_topic_avail(t, sizeof t, "PlantHub-7814"));
    assert(strcmp(t, "planthub/PlantHub-7814/status") == 0);
    assert(mqtt_topic_discovery(t, sizeof t, 7, CAP_SOIL_MOISTURE));
    assert(strcmp(t, "homeassistant/sensor/planthub_plant_7_soil_moisture/config") == 0);
    assert(mqtt_topic_discovery(t, sizeof t, 7, CAP_AIR_TEMPERATURE));
    assert(strcmp(t, "homeassistant/sensor/planthub_plant_7_air_temperature/config") == 0);
    char small[8];
    assert(!mqtt_topic_state(small, sizeof small, "PlantHub-7814", 7));
    assert(!mqtt_topic_discovery(t, sizeof t, 7, CAPABILITY_COUNT));   /* unknown cap_id */

    /* state JSON: exactly the bound (present) capabilities, keyed by the
     * capability's full name, formatted to its table precision -- absent
     * ones (including rssi -- no longer special-cased) are omitted, never
     * emitted as null. */
    mqtt_state_t st = { 0 };
    st.present[CAP_AIR_TEMPERATURE] = true;  st.value[CAP_AIR_TEMPERATURE] = 27.3f;
    st.present[CAP_SOIL_MOISTURE] = true;    st.value[CAP_SOIL_MOISTURE] = 18.0f;
    assert(mqtt_json_state(j, sizeof j, &st));
    assert(strcmp(j, "{\"soil.moisture\":18,\"air.temperature\":27.3}") == 0);

    mqtt_state_t none = { 0 };
    assert(mqtt_json_state(j, sizeof j, &none));
    assert(strcmp(j, "{}") == 0);

    /* Device state: mqtt_json_state() is shape-agnostic (plant vs device is
     * just which topic it's published to, mqtt_pub.c's job) -- this is the
     * exact pairing mqtt_pub.c's publish_device_state() builds: device
     * state topic + a state payload straight off a registry device's raw
     * capability slots (bound to a plant or not, unlike the plant path
     * which only ever includes currently-BOUND capabilities). */
    assert(mqtt_topic_device_state(t, sizeof t, "PlantHub-7814", "ble:A4C138001122"));
    assert(strcmp(t, "planthub/PlantHub-7814/device/ble:A4C138001122/state") == 0);
    mqtt_state_t dev_st = { 0 };
    dev_st.present[CAP_SOIL_MOISTURE] = true;   dev_st.value[CAP_SOIL_MOISTURE] = 42.0f;
    dev_st.present[CAP_BATTERY_LEVEL] = true;   dev_st.value[CAP_BATTERY_LEVEL] = 64.0f;
    assert(mqtt_json_state(j, sizeof j, &dev_st));
    assert(strcmp(j, "{\"soil.moisture\":42,\"battery.level\":64}") == 0);

    /* discovery payload for soil.moisture: unit "%" and the V1 entity-id
     * pattern, metric segment derived from the capability name. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "Palme", CAP_SOIL_MOISTURE));
    assert(strstr(j, "\"uniq_id\":\"planthub_plant_7_soil_moisture\"") != NULL);
    assert(strstr(j, "\"stat_t\":\"planthub/PlantHub-7814/plant/7/state\"") != NULL);
    assert(strstr(j, "\"avty_t\":\"planthub/PlantHub-7814/status\"") != NULL);
    assert(strstr(j, "\"val_tpl\":\"{{ value_json['soil.moisture'] }}\"") != NULL);
    assert(strstr(j, "\"dev_cla\":\"moisture\"") != NULL);
    assert(strstr(j, "\"unit_of_meas\":\"%\"") != NULL);
    assert(strstr(j, "\"suggested_display_precision\":0") != NULL);
    assert(strstr(j, "\"name\":\"Palme soil.moisture\"") != NULL);
    assert(strstr(j, "\"via_device\":\"PlantHub-7814\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"Palme\",") != NULL);

    /* HA unit translation (Task 7 item 1): the table stores "C"/"lux", HA
     * discovery emits "\u00b0C"/"lx". The table itself is untouched -- only
     * the discovery path translates. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "Palme", CAP_AIR_TEMPERATURE));
    assert(strstr(j, "\"unit_of_meas\":\"\\u00b0C\"") != NULL);
    assert(strstr(j, "\"suggested_display_precision\":1") != NULL);
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "Palme", CAP_LIGHT_ILLUMINANCE));
    assert(strstr(j, "\"unit_of_meas\":\"lx\"") != NULL);

    /* no name falls back to "Plant <id>", built inside the discovery
     * builder itself; a capability with no HA device class (conductivity)
     * omits the dev_cla field rather than emitting null/empty. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "", CAP_SOIL_CONDUCTIVITY));
    assert(strstr(j, "\"name\":\"Plant 7 soil.conductivity\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"Plant 7\",") != NULL);
    assert(strstr(j, "dev_cla") == NULL);
    assert(strstr(j, "\"unit_of_meas\":\"uS/cm\"") != NULL);

    assert(!mqtt_json_discovery(j, sizeof j, "h", 1, "", CAPABILITY_COUNT));

    /* a name containing a double quote must not break the JSON: both the
     * "name" field and dev.name escape it. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "a\"b", CAP_AIR_TEMPERATURE));
    assert(strstr(j, "\"name\":\"a\\\"b air.temperature\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_plant_7\"],\"name\":\"a\\\"b\",") != NULL);

    /* a name containing a backslash and a control char is escaped too. */
    assert(mqtt_json_discovery(j, sizeof j, "PlantHub-7814", 7, "a\\b\tc", CAP_AIR_TEMPERATURE));
    assert(strstr(j, "\"name\":\"a\\\\b\\u0009c air.temperature\"") != NULL);

    /* Device-form discovery (spec Sec.6, amended): same shape as the plant
     * form, but identified by the device id string, state topic pointed at
     * the device state topic, and display_name falling back to the id
     * string itself (a device has no numeric "Plant <id>" equivalent).
     * mqtt_json_device_discovery() only ever builds a payload for whatever
     * (device, cap) pair it's given -- the DEDUP decision (skip publishing
     * this at all when the capability is already exposed through a plant
     * binding, per the amended spec) is made by the caller, mqtt_pub.c's
     * publish_device_discovery(), which consults the live plants table;
     * that decision isn't reachable from this host-only layer, so it's not
     * asserted here -- only the payload shape this function produces once
     * the caller has already decided to call it (the "unbound" case). */
    assert(mqtt_topic_device_discovery(t, sizeof t, "ble:A4C138001122", CAP_SOIL_MOISTURE));
    assert(strcmp(t, "homeassistant/sensor/planthub_device_ble:A4C138001122_soil_moisture/config") == 0);
    assert(!mqtt_topic_device_discovery(t, sizeof t, "ble:A4C138001122", CAPABILITY_COUNT));

    assert(mqtt_json_device_discovery(j, sizeof j, "PlantHub-7814", "ble:A4C138001122", "", CAP_SOIL_MOISTURE));
    assert(strstr(j, "\"uniq_id\":\"planthub_device_ble:A4C138001122_soil_moisture\"") != NULL);
    assert(strstr(j, "\"stat_t\":\"planthub/PlantHub-7814/device/ble:A4C138001122/state\"") != NULL);
    assert(strstr(j, "\"avty_t\":\"planthub/PlantHub-7814/status\"") != NULL);
    assert(strstr(j, "\"val_tpl\":\"{{ value_json['soil.moisture'] }}\"") != NULL);
    assert(strstr(j, "\"dev_cla\":\"moisture\"") != NULL);
    assert(strstr(j, "\"unit_of_meas\":\"%\"") != NULL);
    /* empty display_name falls back to the id string itself, both in
     * "name" (paired with the capability name) and in dev.name/dev.ids. */
    assert(strstr(j, "\"name\":\"ble:A4C138001122 soil.moisture\"") != NULL);
    assert(strstr(j, "\"dev\":{\"ids\":[\"planthub_device_ble:A4C138001122\"],"
                     "\"name\":\"ble:A4C138001122\",") != NULL);

    /* a real display name (app_config's optional per-mac sensor name)
     * overrides the id-string fallback. */
    assert(mqtt_json_device_discovery(j, sizeof j, "PlantHub-7814", "ble:A4C138001122",
                                       "Windowsill Probe", CAP_AIR_TEMPERATURE));
    assert(strstr(j, "\"name\":\"Windowsill Probe air.temperature\"") != NULL);
    assert(strstr(j, "\"unit_of_meas\":\"\\u00b0C\"") != NULL);   /* HA unit translation applies here too */

    assert(!mqtt_json_device_discovery(j, sizeof j, "h", "ble:AA", "", CAPABILITY_COUNT));

    /* retained-topic cleanup (spec Sec.6): the payload is always empty --
     * the RETAINED flag itself is set by every mqtt_pub.c cleanup call
     * site's esp_mqtt_client_publish(), not testable at this host-only
     * layer (see MQTT_CLEANUP_PAYLOAD's doc comment). */
    assert(strcmp(MQTT_CLEANUP_PAYLOAD, "") == 0);

    printf("test_mqtt_json: OK\n");
    return 0;
}
