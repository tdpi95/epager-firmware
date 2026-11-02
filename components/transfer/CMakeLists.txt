# Portable transfer state machine, OTA decisions, blob_storage interface, its file-system adapter (blob_fs,
# POSIX only) and the in-memory fakes (blob_mem, fw_mem). Builds as an ESP-IDF component, or as a plain static library for host tests.
set(XFER_SRCS
    src/blob_fs.c
    src/blob_mem.c
    src/fw_mem.c
    src/xfer.c
    src/xfer_ota.c
)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${XFER_SRCS} INCLUDE_DIRS include REQUIRES protocol config_store)
    epager_component_warnings()
else()
    list(TRANSFORM XFER_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_transfer STATIC ${XFER_SRCS})
    target_include_directories(epager_transfer PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_transfer PUBLIC epager_protocol epager_config_store)
endif()
