if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    target_sources(app PRIVATE
        src/custom_status_screen.c
        src/gremlin_art.c
    )
    target_include_directories(app PRIVATE src)
endif()
