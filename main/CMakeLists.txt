idf_component_register(SRCS "main.c" REQUIRES app_config claiming wifi_manager webserver data_core ble_collector zigbee radio_role timekeeper sampler swarm integrations plants event_log rules tuya_dp esp_netif esp_event json
    PRIV_REQUIRES esp_system heap)
