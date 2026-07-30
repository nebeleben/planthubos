idf_component_register(
    SRCS "wifi_fsm.c" "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif app_config
)
