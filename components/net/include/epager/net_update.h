/* What the firmware glue gives the Wi-Fi task for updates (components/net/src/net_update.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epager/xfer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *board_id;           /* the strings must outlive the task */
    uint8_t variant;                /* PROTO_VARIANT_* */
    const char *running_version;
    const char *contract_version;
    const xfer_fw_sink_t *sink;     /* the Wi-Fi view of the firmware sink (ota_sink_t.wifi) */
    uint32_t slot_size;
    const char *user_agent;
} net_update_cfg_t;

/* Before the first update call, after net_start. */
void net_update_start(const net_update_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
