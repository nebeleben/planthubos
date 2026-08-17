idf_component_register(
    SRCS "gatt_fsm.c" "gatt_sched.c" "gatt_engine.c"
    INCLUDE_DIRS "include"
    # gatt_fsm.c and gatt_sched.c are pure C99 and host-tested; only
    # gatt_engine.c (the NimBLE adapter) needs any of this.
    # event_log was here in the first cut of Task 6 and is deliberately
    # gone: the amended spec section 5 cut M5a's per-attempt event-log
    # entry, because no task allowed to write it has a stack that can hold
    # the LittleFS-plus-SSE-hook chain.
    REQUIRES bt esp_timer psvm data_core wrappers
)
