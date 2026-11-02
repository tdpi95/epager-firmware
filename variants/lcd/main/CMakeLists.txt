idf_component_register(SRCS main.c bringup.c
                       PRIV_REQUIRES app ${EPAGER_BOARD} board_api protocol
                                     nvs_flash joltwallet__littlefs esp_timer esp_app_format heap log freertos)
