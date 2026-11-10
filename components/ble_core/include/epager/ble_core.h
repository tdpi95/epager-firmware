/*
 * BLE glue (PLAN D67, ESP-IDF only, NimBLE): the ePager GATT service of contracts/gatt.json, Battery and
 * Device Information, advertising (D7), LE Secure Connections pairing with a DisplayOnly passkey,
 * bonding, the connection policy (D8) and, on iPhones, the ANCS client's radio side (ancs_client).
 *
 * Threads: NimBLE callbacks run in the NimBLE host task and only push records into a FIFO; the app task
 * (which owns the portable core) takes them with blec_poll(). Nothing here calls into the core.
 * Notification content is never logged; peer addresses are not logged.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/ble_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLEC_WRITE_MAX 512u /* ATT attribute value limit (a write) */
#define BLEC_DATA_MAX 520u  /* the most an event carries: a notification at the largest MTU (517 - 3 = 514) fits */

typedef enum {
    BLEC_EV_NONE = 0,
    BLEC_EV_CONNECTED,     /* a phone connected (not yet encrypted): mtu */
    BLEC_EV_DISCONNECTED,  /* mtu holds the disconnect reason (NimBLE's code) */
    BLEC_EV_MTU,           /* the ATT MTU changed: mtu */
    BLEC_EV_SECURE,        /* the link is encrypted, authenticated and bonded: Secure Connections (D6) */
    BLEC_EV_PASSKEY,       /* pairing: show this code (passkey) */
    BLEC_EV_PAIR_DONE,     /* pairing ended: ok */
    BLEC_EV_BONDS,         /* the number of bonded phones changed: bonds */
    BLEC_EV_WRITE,         /* a write to characteristic chr (CORE_CHR_*): data, len (segment header included) */
    BLEC_EV_ANCS_READY,    /* iPhone: Notification Source and Data Source are subscribed (a new ANCS session) */
    BLEC_EV_ANCS_LOST,     /* iPhone: the ANCS session ended (link lost, service changed) */
    BLEC_EV_ANCS_NS,       /* data, len: a Notification Source value */
    BLEC_EV_ANCS_DS,       /* data, len: a Data Source fragment */
    BLEC_EV_ANCS_WRITE,    /* the Control Point write finished: ok = ATT status (0 = success) */
} blec_event_kind_t;

typedef struct {
    blec_event_kind_t kind;
    uint8_t chr;
    uint16_t len, mtu;
    uint32_t passkey;
    uint8_t ok, bonds;
    uint8_t data[BLEC_DATA_MAX];
} blec_event_t;

typedef struct {
    const char *manufacturer, *model, *firmware; /* Device Information (kept by the caller) */
    /* Called from the NimBLE host task after an event was queued: wake the app task (task notification). */
    void (*wake)(void *ctx);
    void *ctx;
    bool ancs;            /* iPhone support: discover and subscribe to the ANCS (PROTO_FEATURES_ANCS) */
} blec_config_t;

/* "ePager-XXXX" from the Bluetooth address (valid before blec_init); the caller puts it in Capabilities. */
const char *blec_device_name(void);

/* Initialise NimBLE (nvs_flash_init() first: the bonds live in NVS) and build the GATT table. The board
 * advertises from blec_start() on. False on failure. */
bool blec_init(const blec_config_t *cfg);

/* Start advertising (after the app has set the read values): until then the host runs but is not visible. */
void blec_start(void);
/* Quiet hours (PLAN D125, protocol.md 5): app task. Stop switches Bluetooth off completely (the link ends with a
 * normal disconnect, NimBLE and the controller are torn down, so the controller no longer keeps the main crystal
 * running in light sleep); BLEC_EV_DISCONNECTED follows if a phone was linked. Resume builds it again and
 * advertises; a bonded phone reconnects by itself. Both block: usually well under a second, a stop up to the
 * supervision timeout (6 s) if the phone does not answer the disconnect. Harmless when already in that state. While stopped, the other calls do nothing (blec_bond_count: the count before the stop). */
void blec_stop(void);
bool blec_resume(void);
bool blec_running(void);

/* App task: the next event, oldest first. False when there is none. */
bool blec_poll(blec_event_t *ev);

/* App task: one value for the phone's Config / Notification / Transfer / Status characteristic (a segment,
 * as core's `notify` gives it). Queued; sent when the link is encrypted and the phone subscribed. */
void blec_notify(uint8_t chr, const uint8_t *val, size_t len);
/* App task: send what is queued (call after every pass of the main loop). True while something waits. */
bool blec_pump(void);

/* App task: the values the phone reads: Capabilities (readable before pairing) and Status. Call after
 * anything they show changes. */
void blec_set_read_value(uint8_t chr, const uint8_t *val, size_t len);
/* Battery Service level, 0-100; 255 = unknown. Notifies on change. */
void blec_set_battery(uint8_t pct);
/* New bonds are accepted only while this is on (PLAN D6). Opening it releases the current link; known phones
 * cannot reclaim the sole connection automatically, so advertising remains available to the new phone (D89). */
void blec_set_pairing_mode(bool on);
/* The app task has transfers or ANCS requests in flight: keep the fast connection interval. */
void blec_set_busy(bool busy);

/* Disconnect the phone, if any. */
void blec_disconnect(void);
/* A/B drain tests (PLAN D106): the idle connection parameters to ask for (NULL = the defaults); false if Apple's
 * rules refuse them. A live link is asked again within a second (fast for 5 s, then these). */
bool blec_set_idle_params(const bleu_conn_params_t *p);
/* A/B drain tests (PLAN D110): the period of the host task's tick (0 = the default 250 ms). It runs the connection
 * mode policy, the advertising interval and the retries, so a longer one only makes them react later. */
void blec_set_tick_ms(uint32_t ms);
/* Forget every bonded phone (and disconnect). */
void blec_forget_bonds(void);
/* Number of bonded phones. */
uint8_t blec_bond_count(void);
/* App task: the bonded phones in the store's order (at most max); connected[i] is true for the one on the
 * link now. Returns how many. */
size_t blec_bonds(bool *connected, size_t max);
/* Forget the bonded phone at `index` of blec_bonds (disconnects it if it is the one on the link). Queued;
 * BLEC_EV_BONDS follows. */
void blec_forget_bond(size_t index);
bool blec_connected(void);

/* iPhone: write a command to the Control Point (the result comes as BLEC_EV_ANCS_WRITE). */
bool blec_ancs_write(const uint8_t *cmd, size_t len);

#ifdef __cplusplus
}
#endif
