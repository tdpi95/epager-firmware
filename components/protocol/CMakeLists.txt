# Portable protocol library. Builds as an ESP-IDF component, or as a plain static
# library for host tests (tests/host).
set(PROTO_SRCS
    src/proto_ancs.c
    src/proto_config.c
    src/proto_gen.c
    src/proto_msg.c
    src/proto_seg.c
    src/proto_sha256.c
    src/proto_tlv.c
    src/proto_util.c
)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${PROTO_SRCS} INCLUDE_DIRS include)
    epager_component_warnings()
else()
    list(TRANSFORM PROTO_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_protocol STATIC ${PROTO_SRCS})
    target_include_directories(epager_protocol PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
endif()
