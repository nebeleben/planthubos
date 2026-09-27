idf_component_register(
    SRCS "dev_profiles.c" "dev_profiles_builtin.c" "dev_profiles_json.c" "mapping_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES capability json tuya_dp
)
