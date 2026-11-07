/* Host harness: the board core on in-memory storage, with captured notifications. */
#pragma once

#include "epager/core.h"

#define XCORE_MAX_OUT 256
#define XCORE_FW_SLOT 4096u

typedef struct {
    uint8_t chr;
    uint16_t len;
    uint8_t v[CORE_MTU_MAX];
} xcore_note_t;

typedef struct {
    uint8_t chr;
    size_t len;
    uint8_t m[PROTO_LIMIT_MAX_MESSAGE_NOTIFICATION];
} xcore_msg_t;

typedef struct {
    /* persistent storage (survives xcore_reboot) */
    kv_mem_t kv;
    blob_mem_t blob;
    fw_mem_t fw;
    uint8_t fw_buf[XCORE_FW_SLOT];
    /* platform state */
    uint32_t now_ms;
    bool has_battery, battery_ok;
    uint8_t battery_pct, external_power;
    bool has_rtc;              /* off by default; rtc_utc = 0: the RTC holds no valid time */
    uint32_t rtc_utc, rtc_at_ms; /* the RTC counts from rtc_utc since now_ms == rtc_at_ms */
    int rtc_writes;
    char version[XFER_OTA_VERSION_MAX + 1];
    bool has_wifi;             /* wifi + weather features */
    proto_status_t wifi_result; /* what the platform's wifi_set returns */
    char wifi_ssid[CORE_SSID_MAX + 1], wifi_pw[64]; /* the selected network ("" = none) */
    char wifi_profiles[3][CORE_SSID_MAX + 1];       /* least to most recently saved/used */
    uint8_t wifi_profile_count;
    int wifi_sets;
    /* RAM */
    npipe_slot_t slots[8];
    npipe_app_t apps[16];
    xfer_asset_support_t assets[3];
    core_params_t params;
    core_t core;
    /* captured outputs */
    xcore_note_t out[XCORE_MAX_OUT];
    int n_out;
    int n_notif_events, n_config_events, ota_events[8];
    npipe_notif_t last_notif;
    int n_dismissed;           /* on_dismissed calls */
    int n_setting[2];          /* on_setting calls by core_setting_t: config, wifi */
    npipe_dismissed_t last_dismissed;
} xcore_t;

/* Board like the Paper: theme assets, firmware with a 4 KiB slot, battery 80 %. Connected
 * (secure, MTU 517) after xcore_new. Allocates; free with xcore_free. */
xcore_t *xcore_new(void);
void xcore_free(xcore_t *x);
/* Simulated reboot: RAM lost, storage kept; reconnects unless connect is false. */
void xcore_reboot(xcore_t *x, bool image_pending_verify, bool connect);
/* Segment msg at the core's MTU and write it to chr. Clears the captured outputs first. */
void xcore_send(xcore_t *x, uint8_t chr, const uint8_t *msg, size_t len);
/* Same, from a logical JSON message (test_support msg_from_json). */
void xcore_send_json(xcore_t *x, uint8_t chr, const char *json);
/* Reassemble the captured notifications into messages (per characteristic, in order). */
int xcore_messages(const xcore_t *x, xcore_msg_t *out, int cap);
/* Compact JSON of the i-th reassembled message ("" if none); caller frees. */
char *xcore_msg_json(const xcore_t *x, int i, uint8_t *chr);
