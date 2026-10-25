idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES zigbee data_core app_config nvs_flash esp_event littlefs uart_link)
