idf_component_register(
    SRCS "boottab.c" "timekeeper.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash
    PRIV_REQUIRES esp_timer esp_netif lwip
)
