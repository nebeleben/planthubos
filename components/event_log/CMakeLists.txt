idf_component_register(
    SRCS "event_ring.c" "event_log.c"
    INCLUDE_DIRS "include"
    REQUIRES timekeeper
)
