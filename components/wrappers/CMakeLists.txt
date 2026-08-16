idf_component_register(
    SRCS "wrapper_index.c" "wrapper_store.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES json
)
