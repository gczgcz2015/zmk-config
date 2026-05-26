if(CONFIG_SHIELD_DACTYL_MANUFORM_6X6_DONGLE)
  zephyr_library()
  zephyr_library_sources(src/bongo_cat_art.c src/bongo_fire_art.c src/dongle_display_overlay.c)
endif()
