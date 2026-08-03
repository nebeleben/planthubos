idf_component_register(
    SRCS "swarm_frame.c" "swarm_store.c" "swarm_buf.c" "espnow_link.c" "pairing.c" "swarm.c" "node_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_event data_core mibeacon json
    PRIV_REQUIRES esp_timer esp_hw_support app_config app_update esp_partition bootloader_support esp_app_format mbedtls
)
