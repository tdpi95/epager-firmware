# Simulated Paper board (host only, never in firmware): board_api implementation with an
# emulated 200x200 1-bit e-paper panel, buttons and battery. Built by tests/host.
if(ESP_PLATFORM)
    message(FATAL_ERROR "boards/sim_epaper_154 is a host-only simulator")
endif()
add_library(epager_board_sim STATIC ${CMAKE_CURRENT_LIST_DIR}/src/sim_epaper.c)
target_include_directories(epager_board_sim PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(epager_board_sim PUBLIC epager_board_api epager_protocol)
