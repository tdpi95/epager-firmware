#include "epager/ble_util.h"

#include <string.h>

#include "epager/proto_gen.h"

/* AD types (Bluetooth Core Specification Supplement) */
enum {
    AD_FLAGS = 0x01,
    AD_UUIDS128_COMPLETE = 0x07,
    AD_NAME_SHORT = 0x08,
    AD_NAME_COMPLETE = 0x09,
    AD_SOLICIT128 = 0x15,
};
#define AD_FLAGS_VALUE 0x06u /* LE general discoverable, BR/EDR not supported */
#define SHORT_NAME "ePager"

void bleu_device_name(char out[BLEU_NAME_MAX], const uint8_t addr[6])
{
    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, PROTO_ADV_NAME_PREFIX, 7);
    out[7] = hex[addr[1] >> 4];
    out[8] = hex[addr[1] & 15];
    out[9] = hex[addr[0] >> 4];
    out[10] = hex[addr[0] & 15];
    out[11] = '\0';
}

static size_t put_ad(uint8_t *out, size_t at, size_t cap, uint8_t type, const uint8_t *v, size_t n)
{
    if (at + 2 + n > cap) {
        return 0;
    }
    out[at] = (uint8_t)(1 + n);
    out[at + 1] = type;
    memcpy(&out[at + 2], v, n);
    return at + 2 + n;
}

size_t bleu_adv_data(uint8_t *out, size_t cap)
{
    static const uint8_t flags[] = {AD_FLAGS_VALUE};
    static const uint8_t ancs[] = {PROTO_UUID_ANCS_SERVICE_LE};
    size_t n = put_ad(out, 0, cap, AD_FLAGS, flags, sizeof(flags));
    n = n ? put_ad(out, n, cap, AD_SOLICIT128, ancs, sizeof(ancs)) : 0;
    return n ? put_ad(out, n, cap, AD_NAME_SHORT, (const uint8_t *)SHORT_NAME, strlen(SHORT_NAME)) : 0;
}

size_t bleu_scan_rsp_data(const char *name, uint8_t *out, size_t cap)
{
    static const uint8_t service[] = {PROTO_UUID_SERVICE_LE};
    size_t n = put_ad(out, 0, cap, AD_NAME_COMPLETE, (const uint8_t *)name, strlen(name));
    return n ? put_ad(out, n, cap, AD_UUIDS128_COMPLETE, service, sizeof(service)) : 0;
}

uint16_t bleu_adv_interval(bool pairing_mode, uint32_t since_start_ms)
{
    return pairing_mode || since_start_ms < BLEU_ADV_FAST_MS ? BLEU_ADV_FAST_UNITS : BLEU_ADV_SLOW_UNITS;
}

bool bleu_passkey(uint32_t random, uint32_t *passkey)
{
    if (random >= 4294u * 1000000u) { /* 4,294,000,000: the largest whole multiple of 10^6 below 2^32 */
        return false;
    }
    *passkey = random % 1000000u;
    return true;
}

bool bleu_pairing_allowed(bool pairing_mode, bool peer_bonded)
{
    return pairing_mode || peer_bonded;
}

bool bleu_initiate_security(bool pairing_mode, bool peer_bonded)
{
    return pairing_mode != peer_bonded;
}

bool bleu_drop_unsecured(uint32_t connected_ms, bool pairing_mode, bool peer_bonded)
{
    const uint32_t limit = pairing_mode ? (peer_bonded ? BLEU_BONDED_PAIRING_GRACE_MS : BLEU_UNSECURED_PAIRING_MS)
                                        : BLEU_UNSECURED_MS;
    return connected_ms >= limit;
}

static bleu_conn_params_t relaxed_override;
static bool relaxed_overridden;

bool bleu_set_relaxed_override(const bleu_conn_params_t *p)
{
    if (p == NULL) {
        relaxed_overridden = false;
        return true;
    }
    if (!bleu_conn_params_apple_ok(p)) {
        return false;
    }
    relaxed_override = *p;
    relaxed_overridden = true;
    return true;
}

void bleu_conn_params(bleu_conn_mode_t mode, bleu_conn_params_t *out)
{
    if (mode == BLEU_CONN_RELAXED && relaxed_overridden) {
        *out = relaxed_override;
        return;
    }
    if (mode == BLEU_CONN_FAST) {
        *out = (bleu_conn_params_t){.itvl_min = 12, .itvl_max = 24, .latency = 0, .timeout = 400}; /* 15-30 ms, 4 s */
    } else {
        /* About one radio wake-up a second when idle, like the Pebble's idle setting (168.75-201.25 ms, latency 4,
         * 6 s), with the minimum on Apple's 15 ms grid: 165-200 ms, latency 4, 6 s (PLAN D123). */
        *out = (bleu_conn_params_t){.itvl_min = 132, .itvl_max = 160, .latency = 4, .timeout = 600};
    }
}

bool bleu_conn_params_apple_ok(const bleu_conn_params_t *p)
{
    const uint32_t min_us = p->itvl_min * 1250u, max_us = p->itvl_max * 1250u, timeout_us = p->timeout * 10000u;
    const uint32_t span_us = max_us * (p->latency + 1u);
    return min_us >= 15000u && min_us % 15000u == 0 &&
           (min_us + 15000u <= max_us || (min_us == 15000u && max_us == 15000u)) &&
           span_us <= 2000000u && span_us * 3u < timeout_us && p->latency <= 30u &&
           timeout_us >= 2000000u && timeout_us <= 6000000u;
}

/* ---------------------------------------------------------------- connection policy */

static bool later(uint32_t a, uint32_t b) /* a after b, tolerant of the 32-bit millisecond wrap */
{
    return (int32_t)(a - b) > 0;
}

void bleu_policy_connect(bleu_conn_policy_t *p, uint32_t now_ms)
{
    p->connected = true;
    /* The central chose the first parameters: `mode` is what we last asked for, so the first step asks for FAST. */
    p->mode = BLEU_CONN_RELAXED;
    p->fast_until_ms = now_ms + BLEU_FAST_AFTER_CONNECT_MS;
    p->quiet_until_ms = now_ms + BLEU_SETTLE_MS;
}

void bleu_policy_disconnect(bleu_conn_policy_t *p)
{
    p->connected = false;
}

void bleu_policy_updated(bleu_conn_policy_t *p, uint32_t now_ms)
{
    const uint32_t until = now_ms + BLEU_QUIET_MS;
    if (later(until, p->quiet_until_ms)) {
        p->quiet_until_ms = until;
    }
}

void bleu_policy_activity(bleu_conn_policy_t *p, uint32_t now_ms)
{
    const uint32_t until = now_ms + BLEU_FAST_AFTER_ACTIVITY_MS;
    if (later(until, p->fast_until_ms)) {
        p->fast_until_ms = until;
    }
}

bool bleu_policy_step(bleu_conn_policy_t *p, uint32_t now_ms, bleu_conn_mode_t *out)
{
    if (!p->connected || !later(now_ms, p->quiet_until_ms)) {
        return false;
    }
    const bleu_conn_mode_t want = later(now_ms, p->fast_until_ms) ? BLEU_CONN_RELAXED : BLEU_CONN_FAST;
    if (want == p->mode) {
        return false;
    }
    p->mode = want;
    *out = want;
    return true;
}

/* ---------------------------------------------------------------- FIFO */

void bleu_fifo_init(bleu_fifo_t *f, uint8_t *buf, uint32_t cap)
{
    *f = (bleu_fifo_t){.buf = buf, .cap = cap};
}

static void put(bleu_fifo_t *f, const uint8_t *src, uint32_t n)
{
    const uint32_t first = n < f->cap - f->tail ? n : f->cap - f->tail;
    memcpy(&f->buf[f->tail], src, first);
    memcpy(f->buf, src + first, n - first);
    f->tail = (f->tail + n) % f->cap;
}

static void get(const bleu_fifo_t *f, uint32_t from, uint8_t *dst, uint32_t n)
{
    const uint32_t first = n < f->cap - from ? n : f->cap - from;
    memcpy(dst, &f->buf[from], first);
    memcpy(dst + first, f->buf, n - first);
}

bool bleu_fifo_push(bleu_fifo_t *f, uint8_t tag, const uint8_t *data, uint16_t len)
{
    if (len > BLEU_FIFO_REC_MAX || f->cap - f->used < BLEU_FIFO_HDR + (uint32_t)len) {
        f->drops++;
        return false;
    }
    const uint8_t hdr[BLEU_FIFO_HDR] = {tag, (uint8_t)len, (uint8_t)(len >> 8)};
    put(f, hdr, sizeof(hdr));
    if (len > 0) {
        put(f, data, len);
    }
    f->used += BLEU_FIFO_HDR + len;
    return true;
}

bool bleu_fifo_peek(const bleu_fifo_t *f, uint8_t *tag, uint8_t *out, uint16_t cap, uint16_t *len)
{
    if (f->used == 0) {
        return false;
    }
    uint8_t hdr[BLEU_FIFO_HDR];
    get(f, f->head, hdr, sizeof(hdr));
    const uint16_t n = (uint16_t)(hdr[1] | (hdr[2] << 8));
    if (n > cap) {
        return false;
    }
    get(f, (f->head + BLEU_FIFO_HDR) % f->cap, out, n);
    *tag = hdr[0];
    *len = n;
    return true;
}

void bleu_fifo_drop(bleu_fifo_t *f)
{
    if (f->used == 0) {
        return;
    }
    uint8_t hdr[BLEU_FIFO_HDR];
    get(f, f->head, hdr, sizeof(hdr));
    const uint32_t n = BLEU_FIFO_HDR + (uint32_t)(hdr[1] | (hdr[2] << 8));
    f->head = (f->head + n) % f->cap;
    f->used -= n;
}

bool bleu_fifo_empty(const bleu_fifo_t *f)
{
    return f->used == 0;
}
