idf_component_register(SRCS "plants_table.c" "plants_migrate.c" "plants.c" INCLUDE_DIRS "include" REQUIRES data_core mibeacon storage app_config PRIV_REQUIRES nvs_flash)
