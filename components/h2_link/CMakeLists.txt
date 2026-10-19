idf_component_register(SRCS "h2_link.c" INCLUDE_DIRS "include"
    REQUIRES swarm swarm_codec uart_framer data_core actors capability esp_driver_uart esp_timer esp_driver_gpio)
