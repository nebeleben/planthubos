idf_component_register(
    SRCS "ble_collector.c" "battery_sched.c" "battery_poll.c" "adv_queue.c"
    INCLUDE_DIRS "include"
    REQUIRES bt data_core mibeacon
    PRIV_REQUIRES esp_timer swarm rules esp_system heap
)
