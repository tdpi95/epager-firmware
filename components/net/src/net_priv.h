/* Inside components/net: state shared by the Wi-Fi task (net.c) and the setup hotspot (net_setup.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "epager/net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define NET_TAG "net"
/* Association plus DHCP: 15 s was too short on a weak office network (-82 dBm, 2026-10-14). */
#define NET_JOIN_TIMEOUT_MS 30000
#define NET_BIT_GOT_IP BIT0
#define NET_BIT_FAIL BIT1
#define NET_BIT_ASSOC BIT2   /* associated with the access point (an address may still not come) */

typedef struct {
    uint32_t (*now_ms)(void);
    EventGroupHandle_t ev;          /* station: got an address, or failed (disc_reason) */
    volatile uint16_t disc_reason;  /* wifi_err_reason_t of the last disconnect */
    esp_netif_t *sta, *ap;
    SemaphoreHandle_t lock;         /* st and gen, read by net_poll from another task */
    net_status_t st;
    uint32_t gen;
    uint8_t language;               /* PROTO_LANGUAGE_*, for the setup page */
    char city[WX_CITY_MAX + 1];     /* weather.city, to prefill the setup page */
} net_shared_t;

extern net_shared_t net;

/* net.c, for the setup hotspot (all on the Wi-Fi task unless noted) */
void net_lock(void);
void net_unlock(void);               /* after changing net.st: net_poll reports it */
/* Add/update a station network (up to three); NULL forgets all. */
bool net_store_network(const char *ssid, const char *pw);
/* Join the strongest saved network in the current mode (STA or AP+STA): true with an address. Otherwise
 * *wifi_state is why (PROTO_WIFI_STATE_AUTH_FAILED, _NOT_FOUND, ...). */
bool net_join(uint32_t timeout_ms, uint8_t *wifi_state);
/* Join one saved network. The hotspot uses this to test the network its form just saved. */
bool net_join_named(const char *ssid, uint32_t timeout_ms, uint8_t *wifi_state);
/* From the page server's task: hand a saved form to the Wi-Fi task. False if it is busy. */
bool net_post_save(const char *ssid, const char *pw, const char *city);
void net_weather_kick(void);

/* net_setup.c, on the Wi-Fi task */
bool net_setup_start(void);
void net_setup_stop(void);
bool net_setup_active(void);
void net_setup_save(const char *ssid, const char *pw, const char *city);
void net_setup_service(void);        /* timeouts and the outcome of a join */
