idf_component_register(
    SRCS "ble_collector.c" "battery_sched.c" "battery_poll.c"
    INCLUDE_DIRS "include"
    REQUIRES bt data_core mibeacon swarm
    PRIV_REQUIRES esp_timer
)
