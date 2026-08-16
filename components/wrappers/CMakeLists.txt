idf_component_register(
    SRCS "wrapper_index.c" "wrapper_store.c" "wrapper_arena.c" "wrapper_exec.c" "unknown_capture.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES json psvm data_core capability
)
