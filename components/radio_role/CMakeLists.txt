idf_component_register(
    SRCS "radio_role.c" "radio_role_str.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
