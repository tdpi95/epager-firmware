# Firmware OTA (M6): the sink the transfer layer writes the image into, and the Wi-Fi update logic (upd_*:
# manifest, daily policy, download). ota_sink.c and upd_*.c are portable and also built for the host tests
# (tests/host/test_ota.c, test_upd.c); ota_esp.c is the esp_ota_* flash and the boot-side calls.
set(OTA_PORTABLE src/ota_sink.c src/upd_manifest.c src/upd_policy.c src/upd_download.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${OTA_PORTABLE} src/ota_esp.c INCLUDE_DIRS include
                           REQUIRES transfer protocol weather
                           PRIV_REQUIRES app_update esp_app_format esp_partition esp_timer log)
    epager_component_warnings()
else()
    list(TRANSFORM OTA_PORTABLE PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_ota STATIC ${OTA_PORTABLE})
    target_include_directories(epager_ota PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_ota PUBLIC epager_transfer epager_weather)
endif()
