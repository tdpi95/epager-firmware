/* Segmentation and reassembly (protocol.md 2.1). Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SEG_FIRST 0x80u
#define PROTO_SEG_LAST 0x40u
#define PROTO_SEG_SEQ_MASK 0x3Fu

/* Sender */
typedef struct {
    const uint8_t *msg;
    size_t len;
    size_t pos;
    size_t max_segment; /* bytes per write, header included (ATT_MTU - 3) */
    uint8_t seq;
    bool done;
} proto_seg_tx_t;

void proto_seg_tx_init(proto_seg_tx_t *tx, const uint8_t *msg, size_t len, size_t max_segment);
/* Write the next segment to out (cap >= max_segment). Returns its length, or 0 when done.
 * An empty message yields one header-only segment. max_segment < 2 yields nothing. */
size_t proto_seg_tx_next(proto_seg_tx_t *tx, uint8_t *out, size_t cap);

/* Receiver */
typedef struct {
    proto_status_t status; /* PROTO_OK = complete message; else SEGMENT_ERROR / TOO_LARGE */
    const uint8_t *msg;    /* valid until the next feed */
    size_t len;
} proto_seg_event_t;

typedef struct {
    uint8_t *buf;
    size_t max_message;
    size_t len;
    uint8_t state;
    uint8_t expected;
} proto_seg_rx_t;

void proto_seg_rx_init(proto_seg_rx_t *rx, uint8_t *buf, size_t max_message);
void proto_seg_rx_reset(proto_seg_rx_t *rx);
/* Feed one written/notified value. Writes up to 2 events; returns how many. */
size_t proto_seg_rx_feed(proto_seg_rx_t *rx, const uint8_t *seg, size_t len, proto_seg_event_t ev[2]);

#ifdef __cplusplus
}
#endif
