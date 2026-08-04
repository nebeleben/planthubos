idf_component_register(
    SRCS "integr_config.c" "lineproto.c" "mqtt_json.c" "mqtt_pub.c" "influx.c"
    INCLUDE_DIRS "include"
    REQUIRES data_core mibeacon timekeeper app_config
    PRIV_REQUIRES mqtt esp_http_client nvs_flash json esp_event esp_timer)
