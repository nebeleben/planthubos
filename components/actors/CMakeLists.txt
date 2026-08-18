idf_component_register(
    SRCS "alert.c" "actor_table.c"
    INCLUDE_DIRS "include"
    REQUIRES actions event_log
)
