idf_component_register(SRCS "uart_link.c" INCLUDE_DIRS "include"
    REQUIRES zigbee swarm_codec uart_framer data_core actors capability esp_driver_uart)
