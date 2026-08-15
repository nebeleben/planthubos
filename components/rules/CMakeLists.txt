idf_component_register(
    SRCS "rules_fsm.c" "rules_store.c" "rules_resolver.c" "rules_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES psvm event_log data_core plants app_config
    PRIV_REQUIRES esp_timer json
)
