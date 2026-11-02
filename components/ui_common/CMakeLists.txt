# UI glue: LVGL on a board_api display, refresh policy, generated fonts, placeholder card UI.
# Builds as an ESP-IDF component (with the lvgl/lvgl 9.5.0 managed component, configured by
# lv_conf/lv_conf.h exactly like the host build), or as a static library for the host simulator
# (tests/host).
set(UI_SRCS src/ui.c src/ui_settings.c src/ui_backend_frame.c src/ui_backend_area.c src/ui_layout_mono.c src/ui_layout_colour.c
             src/ui_icons.c src/ui_weather_icons.c src/ui_mono_icons.c src/ui_emoji.c fonts/ui_font_regular_16.c fonts/ui_font_bold_16.c fonts/ui_font_clock_56.c
             fonts/ui_font_bold_24.c fonts/ui_font_lcd_16.c fonts/ui_font_lcd_20.c fonts/ui_font_lcd_bold_20.c
             fonts/ui_font_lcd_clock_72.c fonts/ui_font_emoji_colour.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${UI_SRCS} src/ui_features.c INCLUDE_DIRS include
                           REQUIRES board_api notification_pipeline protocol transfer weather lvgl__lvgl)
    epager_component_warnings()

    # One LVGL configuration for host and firmware: our lv_conf.h, not the Kconfig menu. The
    # tools/lvgl_conf_check.sh script compares the effective macros of both builds.
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC
        LV_CONF_PATH="${CMAKE_CURRENT_LIST_DIR}/lv_conf/lv_conf.h")
else()
    list(TRANSFORM UI_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_ui_common STATIC ${UI_SRCS})
    target_include_directories(epager_ui_common PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_ui_common PUBLIC lvgl epager_board_api epager_notification_pipeline epager_protocol epager_weather
                          epager_transfer)
endif()
