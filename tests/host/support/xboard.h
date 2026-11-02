/* Host harness: the transfer component on in-memory storage, configured like a transfer.json board. */
#pragma once

#include "cJSON.h"
#include "epager/xfer.h"

#define XBOARD_MAX_OUT 16

typedef struct {
    /* persistent storage (survives xboard_reboot) */
    kv_mem_t kv;
    blob_mem_t blob;
    fw_mem_t fw;
    uint8_t *fw_buf;
    /* configuration */
    xfer_asset_support_t assets[8];
    size_t n_assets;
    bool has_fw, has_reader, battery_readable;
    uint8_t battery_pct, external_power;
    char running_version[XFER_OTA_VERSION_MAX + 1];
    xfer_ota_params_t ota_params;
    xfer_params_t params;
    /* RAM state */
    xfer_ota_t ota;
    xfer_t x;
    /* messages sent by the board for the last request: events first, then the response */
    uint8_t out[XBOARD_MAX_OUT][PROTO_LIMIT_MAX_MESSAGE_TRANSFER];
    size_t out_len[XBOARD_MAX_OUT];
    int n_out;
    int32_t power; /* shared power budget once xboard_power_limit() is called */
    int ota_events[8];
} xboard_t;

/* board: transfer.json board object (already merged). Allocates; free with xboard_free. */
xboard_t *xboard_new(const cJSON *board);
void xboard_free(xboard_t *b);
/* Simulated reboot / power cut: RAM state lost, storage kept, power budget unlimited again. */
void xboard_reboot(xboard_t *b);
void xboard_power_limit(xboard_t *b, int32_t budget);
void xboard_set_battery(xboard_t *b, const cJSON *battery); /* null = no reader */
/* Feed one Transfer-channel message, as the core dispatcher would. */
void xboard_request(xboard_t *b, const uint8_t *msg, size_t len);
/* Deep-merge `over` into a copy of `base` (null values replace). Caller deletes. */
cJSON *json_merge(const cJSON *base, const cJSON *over);
/* The default board of transfer.json merged with overrides given as a JSON string (or NULL). */
xboard_t *xboard_from_vectors(const char *overrides_json);
