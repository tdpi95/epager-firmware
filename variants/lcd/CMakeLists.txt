# LCD firmware (colour LCD boards). Build with
#   idf.py -C variants/lcd -B build/lcd build
# -DEPAGER_BOARD=<dir under boards/> selects another LCD board (variants/common.cmake).
cmake_minimum_required(VERSION 3.16)

set(EPAGER_VARIANT lcd)
set(EPAGER_DEFAULT_BOARD ostb_xiaozhi_3st)
include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epager_lcd)
