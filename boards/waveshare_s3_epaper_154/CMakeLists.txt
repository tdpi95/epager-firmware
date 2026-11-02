# Waveshare ESP32-S3-ePaper-1.54 V2 board (ESP-IDF component, firmware only): board_api
# implementation, pins in src/ws154_pins.h. Docs: docs/adding-a-board.md.
if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "boards/waveshare_s3_epaper_154 builds only inside an ESP-IDF project")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)

idf_component_register(SRCS src/board.c src/ws154_epd.c src/ws154_i2c.c src/ws154_audio.c
                       REQUIRES board_api protocol
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_i2s esp_rom esp_adc esp_timer log freertos)
epager_component_warnings()
