# Portable clock face parser and evaluator (contracts/asset-formats.md "Clock faces"). Builds as an ESP-IDF
# component, or as a plain static library for host tests (tests/host).
set(CF_SRCS src/cf_parse.c src/cf_eval.c src/cf_opts.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${CF_SRCS} INCLUDE_DIRS include REQUIRES protocol)
    epager_component_warnings()
else()
    list(TRANSFORM CF_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_clock_face STATIC ${CF_SRCS})
    target_include_directories(epager_clock_face PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_clock_face PUBLIC epager_protocol)
endif()
