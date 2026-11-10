/*
 * Wi-Fi for the weather (PLAN D60-D64), ESP-IDF glue. One task owns the radio: it joins the stored
 * network only to fetch (components/weather decides when and what), sets the clock from SNTP, and runs
 * the setup hotspot (net_setup.c) on request. The firmware glue talks to it through these calls (any
 * task) and reads what changed with net_poll from its own loop. The password is never logged.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto.h"
#include "epager/wx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NET_SETUP_OFF = 0, NET_SETUP_WAITING, NET_SETUP_JOINING, NET_SETUP_DONE, NET_SETUP_FAILED } net_setup_state_t;

/* Firmware update over Wi-Fi (CONFIG_EPAGER_FEATURE_WIFI_OTA, docs/wifi-ota-plan.md). The policy lives in the
 * main loop (components/ota, upd_policy); the Wi-Fi task checks the manifest and downloads on request. */
typedef enum {
    NET_UPD_IDLE = 0,
    NET_UPD_CHECKING,
    NET_UPD_DOWNLOADING,
    NET_UPD_INSTALLED, /* the image is installed and is the boot image: the board restarts next */
    NET_UPD_FAILED,    /* see upd_error */
} net_upd_state_t;

typedef enum {
    NET_UPD_ERR_NONE = 0,
    NET_UPD_ERR_NO_NETWORK, /* no Wi-Fi network is saved */
    NET_UPD_ERR_NET,        /* could not join, or the release host did not answer */
    NET_UPD_ERR_CLOCK,      /* the time is not set: the certificate cannot be checked */
    NET_UPD_ERR_DOWNLOAD,   /* wrong size or hash, or the connection kept failing */
    NET_UPD_ERR_REJECTED,   /* the image check failed: not our image, or not signed by the trusted key */
    NET_UPD_ERR_BUSY,       /* a firmware transfer from the phone is running */
    NET_UPD_ERR_CANCELLED,
    NET_UPD_ERR_INTERNAL,
} net_upd_error_t;

typedef struct {
    uint8_t wifi_state;            /* PROTO_WIFI_STATE_* */
    char ssid[33];                 /* the stored network, "" = none */
    uint8_t n_networks;            /* the saved networks (up to three), for the Paper's settings page */
    char networks[3][33];
    uint8_t weather_state;         /* PROTO_WEATHER_STATE_* */
    bool have_reading;             /* a reading to show (fresh) */
    wx_reading_t reading;
    uint32_t time_seq;             /* bumped for every network time (SNTP, else an HTTPS reply's Date): */
    uint32_t time_utc, time_ms;    /* UTC seconds at monotonic milliseconds time_ms */
    net_setup_state_t setup;
    char ap_ssid[33], ap_password[17], setup_network[33];
    char setup_url[24];            /* the setup page, e.g. "192.168.4.1" */
    uint32_t city_seq;             /* bumped when the setup page saved a city: */
    char city[WX_CITY_MAX + 1];
    uint8_t upd_state;             /* net_upd_state_t */
    uint8_t upd_error;             /* net_upd_error_t, with NET_UPD_FAILED */
    uint8_t upd_verdict;           /* upd_verdict_t of the last check that got an answer */
    uint8_t upd_pct;               /* DOWNLOADING: 0-100 */
    uint32_t upd_check_seq;        /* bumped when a check ended, with an answer (upd_check_ok) or without */
    bool upd_check_ok;
    char upd_version[33];          /* the release the last check named (also when it is not newer) */
    uint32_t upd_size;             /* its image, bytes */
} net_status_t;

/* Start the Wi-Fi task (after NVS). user_agent goes with every HTTPS request. */
bool net_start(const char *user_agent, uint32_t (*now_ms)(void));
/* Weather settings from config; language: PROTO_LANGUAGE_*. False if the task's queue is full (busy with a
 * fetch): call again later. */
bool net_configure(bool enabled, uint16_t interval_min, const uint8_t *city, size_t city_len, uint8_t language);
/* WIFI_SET (core_params_t.wifi_set): add/update (forget false), forget one (forget + ssid), or forget all
 * (forget without ssid). Queued; returns PROTO_OK, or PROTO_BUSY if the task cannot take it now. */
proto_status_t net_wifi_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *pw, size_t pw_len, bool forget);
/* Open or close the setup hotspot (the pairing screen's button, Close, a key). */
void net_setup(bool open);
/* Fetch now (debug console). */
void net_kick(void);
/* Quiet hours (PLAN D125, any task): while true no scheduled weather fetch starts; a fetch asked for (net_kick, a
 * new network or city) still runs. The app keeps the daily update check back itself. */
void net_set_quiet(bool quiet);
/* Firmware update (any task; queued, false if the Wi-Fi task cannot take it now). A check reads the release
 * manifest, joining Wi-Fi if no weather fetch is under way; manual = the person asked. An install fetches the
 * manifest again, and downloads and installs the image if it still names `version`. Cancel stops a download
 * (checked between blocks). The URL replaces the manifest URL until the next boot (developer builds). */
bool net_update_check(bool manual);
bool net_update_install(const char *version);
void net_update_cancel(void);
void net_update_set_url(const char *manifest_url);
/* Test hook: the next image download loses its connection once after this many bytes (0: off). */
void net_update_debug_drop(uint32_t after_bytes);
/* Copy the status if it changed since the last call. */
bool net_poll(net_status_t *out);

#ifdef __cplusplus
}
#endif
