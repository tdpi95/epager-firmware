# Board abstraction (headers only): BoardProfile and DisplayDriver. Each board under boards/
# implements board_get(). Builds as an ESP-IDF component, or as an INTERFACE library on the host.
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS include)
else()
    add_library(epager_board_api INTERFACE)
    target_include_directories(epager_board_api INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
endif()
