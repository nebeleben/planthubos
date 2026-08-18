idf_component_register(
    SRCS "zigbee.c" "zb_map.c" "zb_store.c" "zb_interview.c" "zb_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES data_core capability actions actors app_config event_log
    PRIV_REQUIRES esp_timer nvs_flash)
