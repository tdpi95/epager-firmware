# ANCS client (PLAN D68). ancs_sm.c is the portable state machine (also built for the host tests,
# tests/host/test_ancs.c); ancs_glue.c is the NimBLE side, ESP-IDF only.
if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS src/ancs_sm.c src/ancs_glue.c INCLUDE_DIRS include
                           REQUIRES protocol notification_pipeline
                           PRIV_REQUIRES bt log)
    epager_component_warnings()
else()
    add_library(epager_ancs_client STATIC ${CMAKE_CURRENT_LIST_DIR}/src/ancs_sm.c)
    target_include_directories(epager_ancs_client PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_ancs_client PUBLIC epager_protocol epager_notification_pipeline)
endif()
