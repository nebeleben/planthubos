idf_component_register(
    SRCS "rtc_ds3231_core.c" "rtc_ds3231.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_event timekeeper
    PRIV_REQUIRES esp_timer)
