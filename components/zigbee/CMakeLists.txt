idf_component_register(
    SRCS "zigbee.c" "zb_map.c" "zb_store.c" "zb_interview.c" "zb_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES data_core capability actions actors app_config event_log
    PRIV_REQUIRES esp_timer nvs_flash)

if(CONFIG_ZB_ENABLED)
    # M6b Task 1 fix round 2 (M0 coex spike's finding, re-confirmed here):
    # espressif__esp-zboss-lib's prebuilt libzboss_stack.zczr.a references
    # zb_zcl_green_power_cluster_handler, which is defined only in
    # espressif__esp-zigbee-lib's prebuilt libesp_zb_api.zczr.a. ESP-IDF's
    # default component link order places the definer before the needer,
    # so a single left-to-right archive scan can never resolve it --
    # "undefined reference to `zb_zcl_green_power_cluster_handler'" at
    # final link, exactly as docs/superpowers/specs/2026-08-15-c5-coex-
    # findings.md's "Constraints discovered" section describes.
    # CONFIG_ZB_GP_ENABLED=n does NOT fix this: the reference is baked
    # into the prebuilt zboss_stack archive unconditionally, whether or
    # not green power is compiled into anything of ours.
    #
    # A throwaway probe ahead of this task built a minimal app (only
    # esp_zb_init()) and linked with zero undefined references, which
    # looked like this bug had gone away in this library version pair.
    # That was a false negative caused by --gc-sections: the minimal app
    # never referenced enough of the stack to keep the needing code path
    # live, so the link never had to resolve the symbol. The full firmware
    # pulls in enough of zboss_stack_lib to make the reference live again,
    # and the M0 finding stands exactly as written. Do not delete this as
    # dead weight because a future probe looks clean without it.
    #
    # -Wl,--start-group/--end-group makes the linker keep re-scanning both
    # archives until no new undefined symbols turn up, instead of the one
    # left-to-right pass that fails above -- it is appended here rather
    # than edited into either library's own link line, so this survives a
    # clean checkout (managed_components/ is downloaded build output and
    # is not tracked by git). Target names are the ones
    # add_prebuilt_library() gives them in the managed components' own
    # CMakeLists.txt: esp_zigbee_api_lib in espressif__esp-zigbee-lib,
    # zboss_stack_lib in espressif__esp-zboss-lib.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--start-group"
        $<TARGET_FILE:esp_zigbee_api_lib>
        $<TARGET_FILE:zboss_stack_lib>
        "-Wl,--end-group")
endif()
