idf_component_register(
    SRCS "swarm_frame.c" "swarm_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
