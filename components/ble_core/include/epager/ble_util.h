/*
 * Portable helpers of the BLE glue (PLAN D67): the advertising bytes, the device name, the passkey, the
 * advertising and connection-parameter policies and a byte FIFO. No NimBLE or FreeRTOS: host-tested
 * (tests/host/test_ble.c), the glue in ble_core.c only calls them. Prefix bleu_.
 *
 * Apple's rules (Accessory Design Guidelines, as quoted in Apple's Technical Q&A QA1931, read 2026-10-14):
 *   advertising: 20 ms for at least 30 s, then one of 152.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5,
 *     1022.5 or 1285 ms, exactly;
 *   connection: Interval Min >= 15 ms and a multiple of 15 ms; Interval Min + 15 ms <= Interval Max (or both
 *     15 ms); Interval Max * (Latency + 1) <= 2 s; Interval Max * (Latency + 1) * 3 < supervision timeout;
 *     Latency <= 30; 2 s <= supervision timeout <= 6 s.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- advertising (protocol.md 1, D7) */

#define BLEU_NAME_MAX 12u /* "ePager-XXXX" and the NUL */
#define BLEU_ADV_MAX 31u

/* "ePager-XXXX": XXXX = the last two bytes of the Bluetooth address, upper-case hex. `addr` is the address
 * as NimBLE holds it: six bytes, least significant first (addr[0] is the last byte of the printed MAC). */
void bleu_device_name(char out[BLEU_NAME_MAX], const uint8_t addr[6]);
/* Advertising data: Flags, the ANCS Service Solicitation (iOS Settings lists the board) and the shortened
 * name "epager" (29 bytes). Returns the length, 0 if cap is too small. */
size_t bleu_adv_data(uint8_t *out, size_t cap);
/* Scan response: the complete name and the ePager service UUID (31 bytes). `name` as from bleu_device_name. */
size_t bleu_scan_rsp_data(const char *name, uint8_t *out, size_t cap);

/* Advertising interval in 0.625 ms units: 20 ms while the board is in pairing mode or for 30 s after
 * advertising (re)started, then 1022.5 ms. Both are on Apple's list. */
#define BLEU_ADV_FAST_UNITS 32u
#define BLEU_ADV_SLOW_UNITS 1636u
#define BLEU_ADV_FAST_MS 30000u
uint16_t bleu_adv_interval(bool pairing_mode, uint32_t since_start_ms);

/* ---------------------------------------------------------------- pairing */

/* A six-digit passkey from 32 random bits. False when `random` falls in the part of the range that would
 * make some numbers more likely than others: draw again. */
bool bleu_passkey(uint32_t random, uint32_t *passkey);

/* New bonds are accepted only in pairing mode (protocol.md 1, PLAN D6); a phone that is already bonded may
 * always encrypt the link again. */
bool bleu_pairing_allowed(bool pairing_mode, bool peer_bonded);

/* Start security ourselves for a new phone in pairing mode, or for a known phone outside it. While pairing
 * mode is open, a known phone gets a short chance to request repeat pairing but is not allowed to reclaim the
 * board automatically and hide its advertisement from the new phone. */
bool bleu_initiate_security(bool pairing_mode, bool peer_bonded);

/* One connection at a time and no advertising while connected, so a phone that connects and never secures
 * the link would lock the board out: it is dropped after 20 s (60 s in pairing mode, when a person is typing
 * a code). A known phone gets only 2 s while pairing mode is open: enough to request repeat pairing, but not
 * enough to hide the board from a new phone. Capabilities can be read in that time (protocol.md 1). */
#define BLEU_UNSECURED_MS 20000u
#define BLEU_UNSECURED_PAIRING_MS 60000u
#define BLEU_BONDED_PAIRING_GRACE_MS 2000u
bool bleu_drop_unsecured(uint32_t connected_ms, bool pairing_mode, bool peer_bonded);

/* ---------------------------------------------------------------- connection parameters (D8) */

typedef struct {
    uint16_t itvl_min, itvl_max; /* 1.25 ms units */
    uint16_t latency;
    uint16_t timeout;            /* 10 ms units */
} bleu_conn_params_t;

typedef enum {
    BLEU_CONN_FAST = 0,    /* 15-30 ms, no latency: transfers, the first seconds, the ANCS replay */
    BLEU_CONN_RELAXED = 1, /* 165-200 ms, latency 4, 6 s: idle (the board skips up to 4 events, a message from the phone waits up to 1 s; D123) */
} bleu_conn_mode_t;

void bleu_conn_params(bleu_conn_mode_t mode, bleu_conn_params_t *out);
/* Apple's rules above. */
bool bleu_conn_params_apple_ok(const bleu_conn_params_t *p);
/* A/B drain tests from the debug console (PLAN D106): ask for these parameters instead of the relaxed ones. NULL =
 * the defaults again. False (and nothing changes) when Apple's rules refuse them. */
bool bleu_set_relaxed_override(const bleu_conn_params_t *p);

#define BLEU_FAST_AFTER_CONNECT_MS 15000u /* discovery, MTU, config sync, ANCS replay */
/* The central often changes the connection parameters itself right after connecting (an iPad did, 2026-10-14).
 * If our own request is pending when its update arrives, ESP-IDF's controller asserts (`lld_con.c`, then the
 * interrupt watchdog resets the chip). So nothing is asked for until the link has been quiet: BLEU_SETTLE_MS
 * after connecting, and again after every parameter change either side made. */
#define BLEU_SETTLE_MS 4000u
#define BLEU_QUIET_MS 1500u
#define BLEU_FAST_AFTER_ACTIVITY_MS 5000u /* transfer writes, ANCS requests */

typedef struct {
    bleu_conn_mode_t mode;
    uint32_t fast_until_ms;
    uint32_t quiet_until_ms; /* no request before this */
    bool connected;
} bleu_conn_policy_t;

void bleu_policy_connect(bleu_conn_policy_t *p, uint32_t now_ms);
void bleu_policy_disconnect(bleu_conn_policy_t *p);
/* The connection parameters changed (the central's own update, or the answer to ours): wait for quiet again. */
void bleu_policy_updated(bleu_conn_policy_t *p, uint32_t now_ms);
/* Traffic that wants the fast interval (extends the window, never shortens it). */
void bleu_policy_activity(bleu_conn_policy_t *p, uint32_t now_ms);
/* True and *out set when the mode the link should use now differs from the one last asked for. */
bool bleu_policy_step(bleu_conn_policy_t *p, uint32_t now_ms, bleu_conn_mode_t *out);

/* ---------------------------------------------------------------- FIFO of tagged byte records */

/* Single-threaded: the glue guards it where two tasks share one. A record is a tag, a length and bytes;
 * pushing into a full FIFO fails and counts a drop. */
typedef struct {
    uint8_t *buf;
    uint32_t cap, head, tail, used;
    uint32_t drops;
} bleu_fifo_t;

#define BLEU_FIFO_REC_MAX 1024u
#define BLEU_FIFO_HDR 3u

void bleu_fifo_init(bleu_fifo_t *f, uint8_t *buf, uint32_t cap);
bool bleu_fifo_push(bleu_fifo_t *f, uint8_t tag, const uint8_t *data, uint16_t len);
/* Copy the oldest record without removing it. False if empty or `cap` is too small for the record. */
bool bleu_fifo_peek(const bleu_fifo_t *f, uint8_t *tag, uint8_t *out, uint16_t cap, uint16_t *len);
/* Remove the oldest record (after a peek). */
void bleu_fifo_drop(bleu_fifo_t *f);
bool bleu_fifo_empty(const bleu_fifo_t *f);

#ifdef __cplusplus
}
#endif
