# Firmware glue shared by the variants (PLAN D54, ESP-IDF only): storage, BLE host, core and UI wiring,
# keys, touch actions and the main loop; the debug console (CONFIG_EPAGER_DEBUG_CONSOLE, D57); the battery
# log (CONFIG_EPAGER_BATTERY_LOG, hw-checklist 27).
if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "components/app builds only inside an ESP-IDF project")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/epager.cmake)

set(voice_files "")
if(CONFIG_EPAGER_SOUND_TEST_VOICE)
    set(voice_files assets/voice_test.pcm) # tools/assets/voice_test.sh
endif()
idf_component_register(SRCS src/app.c src/app_console.c src/app_batlog.c src/app_ab.c src/app_evlog.c src/app_kv.c
                       INCLUDE_DIRS include
                       EMBED_FILES ${voice_files}
                       REQUIRES board_api
                       PRIV_REQUIRES core ble_core ancs_client config_store notification_pipeline protocol transfer ui_common power net weather
                                     bt nvs_flash joltwallet__littlefs console esp_driver_usb_serial_jtag esp_timer esp_app_format esp_pm heap log
                                     freertos)
epager_component_warnings()
