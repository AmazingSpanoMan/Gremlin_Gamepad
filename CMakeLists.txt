cmake_minimum_required(VERSION 3.20.0)

if(CONFIG_SHIELD_GREMLINV3)
    if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM OR CONFIG_ZMK_STUDIO_RPC)
        target_include_directories(app PRIVATE boards/shields/gremlinv3/src)
    endif()

    if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
        target_sources(app PRIVATE
            boards/shields/gremlinv3/src/custom_status_screen.c
            boards/shields/gremlinv3/src/gremlin_art.c
        )
    endif()

    if(CONFIG_ZMK_STUDIO_RPC)
        target_sources(app PRIVATE
            boards/shields/gremlinv3/src/gremlin_rpc.c
        )
    endif()
endif()
