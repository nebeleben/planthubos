#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Topics. hub is the hub name ("PlantHub-7814"), plant_id is the plant's id
 * (plants_table.h -- 1-255, 0 never valid). All return false if the result
 * would not fit. */
bool mqtt_topic_state(char *out, size_t cap, const char *hub, uint8_t plant_id);
/*   -> "planthub/<hub>/plant/<id>/state"                                   */
bool mqtt_topic_avail(char *out, size_t cap, const char *hub);
/*   -> "planthub/<hub>/status"                                             */
bool mqtt_topic_discovery(char *out, size_t cap, uint8_t plant_id, const char *metric);
/*   -> "homeassistant/sensor/planthub_plant_<id>_<metric>/config"          */

/* State payload. Only fields whose has_* flag is set are emitted:
 * {"temp":27.3,"moisture":18,"lux":585,"conductivity":118,"battery":64,"rssi":-88}
 * rssi always emitted. */
typedef struct {
    bool  has_temp, has_moisture, has_lux, has_conductivity, has_battery;
    float temp_c;
    uint8_t moisture_pct; uint32_t lux; uint16_t conductivity; uint8_t battery_pct;
    int8_t rssi;
} mqtt_state_t;
bool mqtt_json_state(char *out, size_t cap, const mqtt_state_t *st);

/* One HA discovery config. metric ∈ {"temp","moisture","lux","conductivity","battery"}.
 * plant_name is the plant's display name or "" -- falls back to "Plant <id>"
 * (built inside this function) when empty.
 * Emits (single line, example for temp of plant 7 named "Palme" on hub PlantHub-7814):
 * {"name":"Palme temp","uniq_id":"planthub_plant_7_temp",
 *  "stat_t":"planthub/PlantHub-7814/plant/7/state",
 *  "avty_t":"planthub/PlantHub-7814/status",
 *  "val_tpl":"{{ value_json.temp }}","dev_cla":"temperature",
 *  "unit_of_meas":"°C","stat_cla":"measurement",
 *  "dev":{"ids":["planthub_plant_7"],"name":"Palme",
 *         "mf":"PlantHub","via_device":"PlantHub-7814"}}
 * Per-metric mapping:
 *   temp         dev_cla temperature  unit °C     val_tpl value_json.temp
 *   moisture     dev_cla moisture     unit %      val_tpl value_json.moisture
 *   lux          dev_cla illuminance  unit lx     val_tpl value_json.lux
 *   conductivity dev_cla (omitted)    unit µS/cm  val_tpl value_json.conductivity
 *   battery      dev_cla battery      unit %      val_tpl value_json.battery
 * Returns false on unknown metric or if it would not fit. */
bool mqtt_json_discovery(char *out, size_t cap, const char *hub, uint8_t plant_id,
                          const char *plant_name, const char *metric);
