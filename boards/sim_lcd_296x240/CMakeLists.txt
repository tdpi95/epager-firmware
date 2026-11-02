# Simulated LCD board (host only, never in firmware): board_api implementation with an emulated
# 296x240 RGB565 LCD on the area path, backlight, touch, keys and battery (PLAN D56). Built by tests/host.
if(ESP_PLATFORM)
    message(FATAL_ERROR "boards/sim_lcd_296x240 is a host-only simulator")
endif()
add_library(epager_board_sim_lcd STATIC ${CMAKE_CURRENT_LIST_DIR}/src/sim_lcd.c)
target_include_directories(epager_board_sim_lcd PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(epager_board_sim_lcd PUBLIC epager_board_api epager_protocol)
