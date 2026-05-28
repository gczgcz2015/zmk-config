if(CONFIG_SHIELD_DACTYL_MANUFORM_6X6_DONGLE)
  zephyr_library()
  zephyr_library_sources(
    src/bongo_cat_art.c
    src/dongle_display_overlay.c
    src/fonts/NerdFonts_Regular_20.c
  )
endif()
