idf_component_register(
    SRCS "alert.c" "alert_ring.c" "actor_table.c" "actor.c" "pending_close.c" "actor_persist.c"
    INCLUDE_DIRS "include"
    REQUIRES actions event_log
    PRIV_REQUIRES esp_timer
)
