if(CONFIG_SHIELD_DACTYL_MANUFORM_6X6_DONGLE)
  zephyr_library()
  zephyr_library_sources(src/dongle_display_overlay.c)
endif()
