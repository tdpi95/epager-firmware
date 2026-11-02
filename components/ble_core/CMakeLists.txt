# BLE glue (PLAN D67, ESP-IDF only) and its portable helpers: the advertising bytes, name, passkey, policies
# and FIFO in ble_util.c are also built for the host tests (tests/host/test_ble.c).
if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS src/ble_util.c src/ble_core.c INCLUDE_DIRS include
                           REQUIRES protocol
                           PRIV_REQUIRES bt nvs_flash esp_timer esp_hw_support freertos log ancs_client)
    epager_component_warnings()
else()
    add_library(epager_ble_util STATIC ${CMAKE_CURRENT_LIST_DIR}/src/ble_util.c)
    target_include_directories(epager_ble_util PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_ble_util PUBLIC epager_protocol)
endif()
