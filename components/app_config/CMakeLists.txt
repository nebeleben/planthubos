idf_component_register(
    SRCS "app_config.c" "power_reset.c" "creds_validate.c" "data_fmt.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES esp_timer esp_hw_support storage
)
