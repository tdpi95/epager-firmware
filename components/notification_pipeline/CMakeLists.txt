# Portable notification pipeline. Builds as an ESP-IDF component, or as a plain static library
# for host tests (tests/host).
set(NPIPE_SRCS src/npipe.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${NPIPE_SRCS} INCLUDE_DIRS include REQUIRES protocol config_store)
    epager_component_warnings()
else()
    list(TRANSFORM NPIPE_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_notification_pipeline STATIC ${NPIPE_SRCS})
    target_include_directories(epager_notification_pipeline PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_notification_pipeline PUBLIC epager_protocol epager_config_store)
endif()
