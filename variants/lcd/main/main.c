/*
 * LCD firmware entry point: the shared firmware glue (components/app, PLAN D54) with LCD's
 * identity; or, with CONFIG_EPAGER_LCD_BRINGUP, the P1a bring-up image (bringup.c) for re-running
 * the hardware checklist.
 */
#include "epager/app.h"
#include "epager/proto_gen.h"
#include "sdkconfig.h"

void bringup_main(void);

void app_main(void)
{
#if CONFIG_EPAGER_LCD_BRINGUP
    bringup_main();
#else
    const app_config_t cfg = {.variant = PROTO_VARIANT_LCD, .product = "ePager LCD", .board = board_get()};
    app_run(&cfg);
#endif
}
