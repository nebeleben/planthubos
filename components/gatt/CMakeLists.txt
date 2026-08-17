idf_component_register(
    SRCS "gatt_fsm.c" "gatt_sched.c" "gatt_engine.c"
    INCLUDE_DIRS "include"
    # gatt_fsm.c and gatt_sched.c are pure C99 and host-tested; only
    # gatt_engine.c (the NimBLE adapter) needs any of this.
    REQUIRES bt esp_timer psvm data_core event_log wrappers
)
