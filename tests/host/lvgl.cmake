# LVGL for the host build, at the version pinned for the firmware (docs/MILESTONES.md, M3).
# The Espressif component registry archive is downloaded once into .deps/ (SHA-256 checked);
# set EPAGER_LVGL_DIR to use an unpacked copy elsewhere.
set(EPAGER_LVGL_VERSION 9.5.0)
set(EPAGER_LVGL_SHA256 95bd2126bdcc8ccd3407eff7c9e76687e41a8a4312bac1538b3d644b461c07fb)
set(EPAGER_LVGL_URL
    https://components-file.espressif.com/components/lvgl/lvgl/${EPAGER_LVGL_VERSION}/lvgl__lvgl-v${EPAGER_LVGL_VERSION}.zip)
set(EPAGER_LVGL_DIR ${REPO_ROOT}/.deps/lvgl-${EPAGER_LVGL_VERSION} CACHE PATH "Unpacked LVGL source")

if(NOT EXISTS ${EPAGER_LVGL_DIR}/lvgl.h)
    set(_zip ${REPO_ROOT}/.deps/lvgl__lvgl-v${EPAGER_LVGL_VERSION}.zip)
    if(NOT EXISTS ${_zip})
        message(STATUS "Downloading LVGL ${EPAGER_LVGL_VERSION} (about 100 MB, once)")
        file(DOWNLOAD ${EPAGER_LVGL_URL} ${_zip}.part EXPECTED_HASH SHA256=${EPAGER_LVGL_SHA256} STATUS _st)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "LVGL download failed: ${_st}. Configure with -DEPAGER_WITH_UI=OFF to skip the UI.")
        endif()
        file(RENAME ${_zip}.part ${_zip})
    endif()
    file(SHA256 ${_zip} _sha)
    if(NOT _sha STREQUAL EPAGER_LVGL_SHA256)
        message(FATAL_ERROR "${_zip}: SHA-256 ${_sha} does not match the pinned ${EPAGER_LVGL_SHA256}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${_zip} DESTINATION ${EPAGER_LVGL_DIR})
endif()

file(GLOB_RECURSE LVGL_SRCS CONFIGURE_DEPENDS ${EPAGER_LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl SYSTEM PUBLIC ${EPAGER_LVGL_DIR} ${REPO_ROOT}/components/ui_common/lv_conf)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w) # third-party code, not held to our warning flags
