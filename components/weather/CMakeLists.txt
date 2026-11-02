# Portable weather logic (PLAN D64). Builds as an ESP-IDF component, or as a plain static library
# for host tests (tests/host).
set(WX_SRCS src/wx.c src/wx_json.c)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)
    idf_component_register(SRCS ${WX_SRCS} INCLUDE_DIRS include REQUIRES protocol)
    epager_component_warnings()
else()
    list(TRANSFORM WX_SRCS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
    add_library(epager_weather STATIC ${WX_SRCS})
    target_include_directories(epager_weather PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(epager_weather PUBLIC epager_protocol)
endif()
