idf_component_register(
    SRCS "authtok.c" "claim.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config nvs_flash
    PRIV_REQUIRES mbedtls esp_driver_gpio esp_hw_support
)
