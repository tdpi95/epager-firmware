# Portable board core (dispatcher, events, read values, soft clock). Builds as an ESP-IDF
# component, or as a plain static library for host tests (tests/host).
set(CORE_SRCS src/core.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${CORE_SRCS} INCLUDE_DIRS include
                           REQUIRES protocol config_store notification_pipeline transfer)
    epager_component_warnings()
else()
    list(TRANSFORM CORE_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_core STATIC ${CORE_SRCS})
    target_include_directories(epager_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_core PUBLIC epager_protocol epager_config_store epager_notification_pipeline
                          epager_transfer)
endif()
