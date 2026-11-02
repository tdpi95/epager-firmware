# Project setup shared by every variant. A variant's CMakeLists.txt sets EPAGER_VARIANT and
# EPAGER_DEFAULT_BOARD, includes this file, then includes ESP-IDF's project.cmake and calls project().
# A board is a folder under boards/ (docs/adding-a-board.md) that brings its own board.cmake,
# sdkconfig.board and partitions.csv, and names its chip; -DEPAGER_BOARD=<folder> builds the variant for another board.

set(EPAGER_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
if(NOT DEFINED EPAGER_BOARD)
    set(EPAGER_BOARD ${EPAGER_DEFAULT_BOARD})
endif()
set(EPAGER_BOARD_DIR ${EPAGER_ROOT}/boards/${EPAGER_BOARD})
if(NOT EXISTS ${EPAGER_BOARD_DIR}/board.cmake)
    message(FATAL_ERROR "No firmware board '${EPAGER_BOARD}' under boards/ (board.cmake missing)")
endif()
include(${EPAGER_BOARD_DIR}/board.cmake) # sets EPAGER_BOARD_VARIANT and EPAGER_BOARD_TARGET
if(NOT EPAGER_BOARD_VARIANT STREQUAL EPAGER_VARIANT)
    message(FATAL_ERROR "Board '${EPAGER_BOARD}' is a ${EPAGER_BOARD_VARIANT} board, this is the ${EPAGER_VARIANT} variant")
endif()
set(EPAGER_TARGET_SDKCONFIG ${CMAKE_CURRENT_LIST_DIR}/targets/${EPAGER_BOARD_TARGET}.sdkconfig)
if(NOT EXISTS ${EPAGER_TARGET_SDKCONFIG})
    message(FATAL_ERROR "Board '${EPAGER_BOARD}' uses chip '${EPAGER_BOARD_TARGET}': variants/targets/${EPAGER_BOARD_TARGET}.sdkconfig is missing")
endif()

set(EXTRA_COMPONENT_DIRS ${EPAGER_ROOT}/components ${EPAGER_BOARD_DIR})

# One sdkconfig per build directory (keeps the source tree clean). Later files win:
# shared, chip, variant, board, the board's partition table, then the optional overlays.
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
file(RELATIVE_PATH EPAGER_PARTITIONS ${CMAKE_CURRENT_SOURCE_DIR} ${EPAGER_BOARD_DIR}/partitions.csv)
set(EPAGER_BOARD_PARTITIONS_FRAGMENT ${CMAKE_BINARY_DIR}/partitions.sdkconfig)
file(WRITE ${EPAGER_BOARD_PARTITIONS_FRAGMENT}
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${EPAGER_PARTITIONS}\"\n"
     "CONFIG_PARTITION_TABLE_FILENAME=\"${EPAGER_PARTITIONS}\"\n")
set(SDKCONFIG_DEFAULTS
    ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.common
    ${EPAGER_TARGET_SDKCONFIG}
    ${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults
    ${EPAGER_BOARD_DIR}/sdkconfig.board
    ${EPAGER_BOARD_PARTITIONS_FRAGMENT})
# Signed images (PLAN D12, option A): -DEPAGER_SIGN=ON. keys/ is git-ignored (docs/recovery.md).
if(EPAGER_SIGN)
    list(APPEND SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.signed)
endif()
# Extra sdkconfig fragment (tools/feature_check.sh builds with the gated features on this way).
if(EPAGER_EXTRA_SDKCONFIG)
    list(APPEND SDKCONFIG_DEFAULTS ${EPAGER_EXTRA_SDKCONFIG})
endif()
