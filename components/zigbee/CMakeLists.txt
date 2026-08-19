idf_component_register(
    SRCS "zigbee.c" "zb_map.c" "zb_store.c" "zb_interview.c" "zb_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES data_core capability actions actors app_config event_log
    PRIV_REQUIRES esp_timer nvs_flash esp_coex heap esp_system)

if(CONFIG_ZB_ENABLED AND CONFIG_ZB_ZCZR)
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
    # is not tracked by git).
    #
    # Fix round 3: cannot reference these archives by add_prebuilt_library()
    # target name (esp_zigbee_api_lib / zboss_stack_lib) from here --
    # add_prebuilt_library() makes them IMPORTED targets, and CMake IMPORTED
    # targets are directory-scoped unless declared GLOBAL, so a name created
    # inside espressif__esp-zigbee-lib's/espressif__esp-zboss-lib's own
    # CMakeLists.txt does not exist in components/zigbee's scope -- do not
    # retry $<TARGET_FILE:...> on them. Instead, build the absolute archive
    # paths from each component's own COMPONENT_DIR the same way their own
    # CMakeLists.txt do, and pass the plain paths on the link line -- an
    # ordinary linker usage that needs no target visibility at all. Gated
    # on CONFIG_ZB_ZCZR (already required, now also spelled out in the
    # if()) because ".zczr" is only the correct archive variant for the
    # coordinator/router build these paths hardcode; if that Kconfig ever
    # goes off, this block must not run rather than link a mismatched
    # variant name that does not exist on disk.
    idf_component_get_property(zboss_lib_dir espressif__esp-zboss-lib COMPONENT_DIR)
    idf_component_get_property(zigbee_lib_dir espressif__esp-zigbee-lib COMPONENT_DIR)
    set(zboss_stack_archive "${zboss_lib_dir}/lib/${IDF_TARGET}/libzboss_stack.zczr.a")
    set(zigbee_api_archive "${zigbee_lib_dir}/lib/${IDF_TARGET}/libesp_zb_api.zczr.a")

    # Fail loudly rather than silently link without the group: a future
    # library version that moves or renames either archive would otherwise
    # resurrect exactly the undefined-reference error this block exists to
    # fix, with nothing pointing at why.
    if(NOT EXISTS "${zboss_stack_archive}")
        message(FATAL_ERROR "components/zigbee: expected archive not found: ${zboss_stack_archive} (espressif__esp-zboss-lib may have moved/renamed its .zczr archive; update the --start-group workaround in components/zigbee/CMakeLists.txt)")
    endif()
    if(NOT EXISTS "${zigbee_api_archive}")
        message(FATAL_ERROR "components/zigbee: expected archive not found: ${zigbee_api_archive} (espressif__esp-zigbee-lib may have moved/renamed its .zczr archive; update the --start-group workaround in components/zigbee/CMakeLists.txt)")
    endif()

    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--start-group"
        "${zigbee_api_archive}"
        "${zboss_stack_archive}"
        "-Wl,--end-group")
endif()
