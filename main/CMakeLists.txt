idf_component_register(SRCS "main.c" REQUIRES app_config claiming wifi_manager webserver data_core ble_collector timekeeper sampler swarm integrations plants event_log rules esp_netif esp_event)
