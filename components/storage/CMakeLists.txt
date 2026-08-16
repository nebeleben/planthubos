idf_component_register(
    SRCS "storage.c" "hourly_agg.c" "storage_compat.c"
    INCLUDE_DIRS "include"
    REQUIRES capability
)
