/*
 * ANCS client, radio side (PLAN D68, ESP-IDF, NimBLE GATT client): discover Apple's Notification Center
 * Service on the iPhone, subscribe to Data Source and Notification Source, write the Control Point, and
 * rediscover when the iPhone's GATT database changes (Service Changed) or ANCS was not there yet (iOS
 * offers it only after bonding and the user's consent). Everything here runs in the NimBLE host task and
 * hands bytes to the app task through `post`; ancs_sm.c turns them into notifications. Prefix ancsg_.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANCSG_EV_READY = 1, /* both characteristics subscribed: a new ANCS session */
    ANCSG_EV_LOST,      /* the session ended */
    ANCSG_EV_NS,        /* a Notification Source value */
    ANCSG_EV_DS,        /* a Data Source fragment */
    ANCSG_EV_WRITE,     /* a Control Point write finished: data[0] = ATT status, 0 = success */
} ancsg_event_t;

typedef void (*ancsg_post_fn)(ancsg_event_t ev, const uint8_t *data, size_t len);

void ancsg_init(ancsg_post_fn post);
/* A/B drain tests (PLAN D106): the longest wait between discovery retries while linked, seconds (default 120);
 * 0 = never retry after a failed discovery (a phone without ANCS). */
void ancsg_set_retry_max_s(uint32_t s);
/* The link to `conn` is encrypted and bonded (host task): discover and subscribe. */
void ancsg_start(uint16_t conn);
/* The link is gone, or the phone is not an iPhone user's any more (host task). */
void ancsg_stop(void);
/* A notification or indication arrived from the phone. True if it was ANCS's or Service Changed's. */
bool ancsg_on_notify_rx(uint16_t conn, uint16_t attr_handle, const uint8_t *data, uint16_t len);
/* Write a Control Point command (any task). The result comes as ANCSG_EV_WRITE. */
bool ancsg_write_cp(const uint8_t *cmd, size_t len);
/* ANCS discovered and subscribed. */
bool ancsg_ready(void);

#ifdef __cplusplus
}
#endif
