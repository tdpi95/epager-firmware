# Wi-Fi for the weather (PLAN D60-D64): the station on demand, HTTPS, SNTP and the setup hotspot. The
# ESP-IDF glue (net.c, net_setup.c) builds only as an ESP-IDF component; net_util.c is portable and is
# also built for the host tests (tests/host).
if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    set(NET_SRCS src/net_util.c)
    if(CONFIG_EPAGER_FEATURE_WIFI)
        list(APPEND NET_SRCS src/net.c src/net_setup.c)
        if(CONFIG_EPAGER_FEATURE_WIFI_OTA)
            list(APPEND NET_SRCS src/net_update.c)
        endif()
    endif()
    idf_component_register(SRCS ${NET_SRCS} INCLUDE_DIRS include
                           REQUIRES protocol weather transfer
                           PRIV_REQUIRES board_api power ota esp_wifi esp_netif esp_event esp_http_client esp_http_server
                                         mbedtls lwip esp_timer nvs_flash)
    epager_component_warnings()
else()
    add_library(epager_net_util STATIC ${CMAKE_CURRENT_LIST_DIR}/src/net_util.c)
    target_include_directories(epager_net_util PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_net_util PUBLIC epager_protocol PRIVATE epager_board_api) # its calendar
endif()
