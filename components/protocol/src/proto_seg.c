#include "epager/proto_seg.h"

#include <string.h>

enum { RX_IDLE, RX_ASSEMBLING, RX_DISCARDING };

void proto_seg_tx_init(proto_seg_tx_t *tx, const uint8_t *msg, size_t len, size_t max_segment)
{
    memset(tx, 0, sizeof(*tx));
    tx->msg = msg;
    tx->len = len;
    tx->max_segment = max_segment;
    tx->done = max_segment < 2;
}

size_t proto_seg_tx_next(proto_seg_tx_t *tx, uint8_t *out, size_t cap)
{
    if (tx->done || cap < tx->max_segment) {
        return 0;
    }
    size_t room = tx->max_segment - 1;
    size_t n = tx->len - tx->pos;
    if (n > room) {
        n = room;
    }
    uint8_t hdr = (uint8_t)(tx->seq & PROTO_SEG_SEQ_MASK);
    if (tx->pos == 0) {
        hdr |= PROTO_SEG_FIRST;
    }
    if (tx->pos + n == tx->len) {
        hdr |= PROTO_SEG_LAST;
        tx->done = true;
    }
    out[0] = hdr;
    if (n) {
        memcpy(out + 1, tx->msg + tx->pos, n);
    }
    tx->pos += n;
    tx->seq = (uint8_t)((tx->seq + 1) & PROTO_SEG_SEQ_MASK);
    return n + 1;
}

void proto_seg_rx_init(proto_seg_rx_t *rx, uint8_t *buf, size_t max_message)
{
    rx->buf = buf;
    rx->max_message = max_message;
    proto_seg_rx_reset(rx);
}

void proto_seg_rx_reset(proto_seg_rx_t *rx)
{
    rx->len = 0;
    rx->state = RX_IDLE;
    rx->expected = 0;
}

static size_t error(proto_seg_rx_t *rx, proto_seg_event_t *ev, size_t n, proto_status_t st)
{
    rx->state = RX_DISCARDING;
    ev[n].status = st;
    ev[n].msg = NULL;
    ev[n].len = 0;
    return n + 1;
}

size_t proto_seg_rx_feed(proto_seg_rx_t *rx, const uint8_t *seg, size_t len, proto_seg_event_t ev[2])
{
    size_t n = 0;
    if (len == 0) {
        if (rx->state == RX_ASSEMBLING) {
            rx->state = RX_DISCARDING;
        }
        ev[0] = (proto_seg_event_t){PROTO_SEGMENT_ERROR, NULL, 0};
        return 1;
    }
    uint8_t hdr = seg[0];
    uint8_t seq = hdr & PROTO_SEG_SEQ_MASK;
    const uint8_t *payload = seg + 1;
    size_t plen = len - 1;

    if (hdr & PROTO_SEG_FIRST) {
        if (rx->state == RX_ASSEMBLING) {
            ev[n++] = (proto_seg_event_t){PROTO_SEGMENT_ERROR, NULL, 0}; /* previous partial dropped */
        }
        if (seq != 0) {
            return error(rx, ev, n, PROTO_SEGMENT_ERROR);
        }
        rx->len = 0;
        rx->expected = 0;
        rx->state = RX_ASSEMBLING;
    } else if (rx->state == RX_DISCARDING) {
        return 0;
    } else if (rx->state == RX_IDLE || seq != rx->expected) {
        return error(rx, ev, n, PROTO_SEGMENT_ERROR);
    }

    if (plen > rx->max_message - rx->len) {
        return error(rx, ev, n, PROTO_TOO_LARGE);
    }
    if (plen) {
        memcpy(rx->buf + rx->len, payload, plen);
    }
    rx->len += plen;
    rx->expected = (uint8_t)((rx->expected + 1) & PROTO_SEG_SEQ_MASK);
    if (hdr & PROTO_SEG_LAST) {
        rx->state = RX_IDLE;
        if (rx->len == 0) {
            ev[n++] = (proto_seg_event_t){PROTO_SEGMENT_ERROR, NULL, 0};
        } else {
            ev[n++] = (proto_seg_event_t){PROTO_OK, rx->buf, rx->len};
        }
    }
    return n;
}
