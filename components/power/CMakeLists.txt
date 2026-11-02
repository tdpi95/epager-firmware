# Power management skeleton (ESP-IDF glue; no host build).
if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS src/power.c INCLUDE_DIRS include PRIV_REQUIRES esp_pm log)
    epager_component_warnings()
endif()
