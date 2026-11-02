/*
 * Paper firmware entry point: the shared firmware glue (components/app, PLAN D54) with the
 * Paper's identity. Everything else comes from the board (boards/waveshare_s3_epaper_154).
 */
#include "epager/app.h"
#include "epager/proto_gen.h"

void app_main(void)
{
    const app_config_t cfg = {.variant = PROTO_VARIANT_PAPER, .product = "ePager Paper", .board = board_get()};
    app_run(&cfg);
}
