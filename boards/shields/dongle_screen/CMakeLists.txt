if(CONFIG_SHIELD_DONGLE_SCREEN)
  zephyr_library()
  # LVGL 9 dropped LV_COLOR_16_SWAP (Kconfig + conf). This ST7789 needs RGB565
  # bytes swapped (the old build set LV_COLOR_16_SWAP=y). The stock Zephyr LVGL
  # glue and mipi-dbi driver do NOT swap, and the ST7789V_RGB565/BGR565 Kconfig
  # choice is a no-op in the data path. The only swap left in LVGL 9.3 is the v8
  # compat hook in lv_refr.c gated on this compile define, so inject it here.
  zephyr_compile_definitions(LV_COLOR_16_SWAP=1)
  zephyr_library_sources(${ZEPHYR_BASE}/misc/empty_file.c)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_include_directories(${ZEPHYR_BASE}/lib/gui/lvgl/)
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_CMAKE_DIR}/include)
  zephyr_library_include_directories(include)
  zephyr_library_sources(src/brightness.c)
  zephyr_library_sources(src/custom_status_screen.c)
  zephyr_library_sources(src/widgets/output_status.c)
  zephyr_library_sources(src/widgets/battery_status.c)
  zephyr_library_sources(src/widgets/layer_status.c)
  zephyr_library_sources(src/widgets/wpm_status.c)
  zephyr_library_sources(src/widgets/mod_status.c)
  file(GLOB font_sources src/fonts/*.c)
  zephyr_library_sources(${font_sources})
endif()