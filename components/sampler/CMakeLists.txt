idf_component_register(
    SRCS "sampler.c"
    INCLUDE_DIRS "include"
    REQUIRES data_core storage timekeeper
    PRIV_REQUIRES esp_timer
)
