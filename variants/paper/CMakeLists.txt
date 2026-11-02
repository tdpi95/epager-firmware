# Paper firmware (e-paper boards). Build with
#   idf.py -C variants/paper -B build/paper build
# -DEPAGER_BOARD=<dir under boards/> selects another paper board (variants/common.cmake).
cmake_minimum_required(VERSION 3.16)

set(EPAGER_VARIANT paper)
set(EPAGER_DEFAULT_BOARD waveshare_s3_epaper_154)
include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epager_paper)
