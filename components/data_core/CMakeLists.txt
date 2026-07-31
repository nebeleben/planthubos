idf_component_register(
    SRCS "registry.c" "data_core.c"
    INCLUDE_DIRS "include"
    REQUIRES mibeacon esp_event
    PRIV_REQUIRES esp_timer
)
