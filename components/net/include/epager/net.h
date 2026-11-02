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
/* Copy the status if it changed since the last call. */
bool net_poll(net_status_t *out);

#ifdef __cplusplus
}
#endif
