if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM OR CONFIG_ZMK_STUDIO_RPC)
    target_include_directories(app PRIVATE src)
endif()

if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    target_sources(app PRIVATE
        src/custom_status_screen.c
        src/gremlin_art.c
    )
endif()

if(CONFIG_ZMK_STUDIO_RPC)
    target_sources(app PRIVATE
        src/gremlin_rpc.c
    )
endif()
