/*
 * Firmware glue shared by the variants (PLAN D54): each variant's app_main calls app_run with what
 * makes it that product. ESP-IDF only.
 */
#pragma once

#include <stdint.h>

#include "epager/board_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t variant;      /* PROTO_VARIANT_* (decides the config keys and Capabilities) */
    const char *product;  /* name in the boot log */
    const board_profile_t *board; /* board_get() of the variant's board (the app does not link a board) */
} app_config_t;

/* Boot the board, storage, BLE host, core and UI, then run the main loop. Does not return. */
void app_run(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
