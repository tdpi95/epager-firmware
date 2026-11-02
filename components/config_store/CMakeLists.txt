# Portable config store + kv_storage interface (and its in-memory fake). Builds as an ESP-IDF
# component, or as a plain static library for host tests (tests/host).
set(CFG_SRCS
    src/cfg_store.c
    src/kv_mem.c
)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${CFG_SRCS} INCLUDE_DIRS include REQUIRES protocol)
    epager_component_warnings()
else()
    list(TRANSFORM CFG_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_config_store STATIC ${CFG_SRCS})
    target_include_directories(epager_config_store PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_config_store PUBLIC epager_protocol)
endif()
