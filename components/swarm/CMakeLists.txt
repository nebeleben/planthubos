idf_component_register(
    SRCS "swarm_frame.c" "swarm_store.c" "espnow_link.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi
    PRIV_REQUIRES esp_timer
)
