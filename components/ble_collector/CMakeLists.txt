idf_component_register(
    SRCS "ble_collector.c" "battery_sched.c" "battery_poll.c" "adv_queue.c"
    INCLUDE_DIRS "include"
    REQUIRES bt data_core mibeacon bthome
    PRIV_REQUIRES esp_timer swarm rules wrappers gatt actors esp_system heap
)
