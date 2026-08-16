idf_component_register(
    SRCS "bthome.c" "bindkey.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES capability mbedtls nvs_flash
)
