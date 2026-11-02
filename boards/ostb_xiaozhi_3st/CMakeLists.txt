# ostb-xiaozhi-3st board (LCD; ESP-IDF component, firmware only): board_api implementation, pins
# in src/xz3_pins.h. Hardware notes: docs/ostb-xiaozhi-3st.md. Docs: docs/adding-a-board.md.
if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "boards/ostb_xiaozhi_3st builds only inside an ESP-IDF project")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)

idf_component_register(SRCS src/board.c src/xz3_lcd.c src/xz3_touch.c
                       REQUIRES board_api protocol
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc esp_lcd esp_adc
                                     esp_timer esp_hw_support heap log freertos)
epager_component_warnings()
