#include "epager/proto_tlv.h"

#include <string.h>

void proto_tlv_reader_init(proto_tlv_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

bool proto_tlv_next(proto_tlv_reader_t *r, proto_tlv_t *out, proto_status_t *err)
{
    *err = PROTO_OK;
    if (r->pos >= r->len) {
        return false;
    }
    size_t left = r->len - r->pos;
    if (left < PROTO_TLV_HDR_LEN) {
        *err = PROTO_MALFORMED;
        return false;
    }
    const uint8_t *p = r->buf + r->pos;
    uint16_t vlen = proto_rd16(p + 1);
    if (p[0] == 0 || (size_t)vlen > left - PROTO_TLV_HDR_LEN) {
        *err = PROTO_MALFORMED;
        return false;
    }
    out->tag = p[0];
    out->len = vlen;
    out->val = p + PROTO_TLV_HDR_LEN;
    r->pos += PROTO_TLV_HDR_LEN + vlen;
    return true;
}

void proto_writer_init(proto_writer_t *w, uint8_t *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
}

static uint8_t *reserve(proto_writer_t *w, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return NULL;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

void proto_w_header(proto_writer_t *w, uint8_t type, uint16_t req_id)
{
    uint8_t *p = reserve(w, PROTO_HEADER_LEN);
    if (p) {
        p[0] = PROTO_MAJOR;
        p[1] = type;
        proto_wr16(p + 2, req_id);
    }
}

void proto_w_bytes(proto_writer_t *w, uint8_t tag, const void *data, size_t len)
{
    if (len > UINT16_MAX) {
        w->overflow = true;
        return;
    }
    uint8_t *p = reserve(w, PROTO_TLV_HDR_LEN + len);
    if (p) {
        p[0] = tag;
        proto_wr16(p + 1, (uint16_t)len);
        if (len) {
            memcpy(p + PROTO_TLV_HDR_LEN, data, len);
        }
    }
}

void proto_w_u8(proto_writer_t *w, uint8_t tag, uint8_t v) { proto_w_bytes(w, tag, &v, 1); }

void proto_w_u16(proto_writer_t *w, uint8_t tag, uint16_t v)
{
    uint8_t b[2];
    proto_wr16(b, v);
    proto_w_bytes(w, tag, b, 2);
}

void proto_w_u32(proto_writer_t *w, uint8_t tag, uint32_t v)
{
    uint8_t b[4];
    proto_wr32(b, v);
    proto_w_bytes(w, tag, b, 4);
}

void proto_w_i16(proto_writer_t *w, uint8_t tag, int16_t v) { proto_w_u16(w, tag, (uint16_t)v); }
void proto_w_i32(proto_writer_t *w, uint8_t tag, int32_t v) { proto_w_u32(w, tag, (uint32_t)v); }
void proto_w_bool(proto_writer_t *w, uint8_t tag, bool v) { proto_w_u8(w, tag, v ? 1 : 0); }
void proto_w_str(proto_writer_t *w, uint8_t tag, const char *s) { proto_w_bytes(w, tag, s, strlen(s)); }

void proto_w_struct_begin(proto_writer_t *w, uint8_t tag)
{
    if (w->depth >= PROTO_WRITER_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    size_t at = w->len;
    uint8_t *p = reserve(w, PROTO_TLV_HDR_LEN);
    if (p) {
        p[0] = tag;
        w->open[w->depth++] = at;
    }
}

void proto_w_struct_end(proto_writer_t *w)
{
    if (w->overflow || w->depth == 0) {
        w->overflow = true;
        return;
    }
    size_t at = w->open[--w->depth];
    size_t vlen = w->len - at - PROTO_TLV_HDR_LEN;
    if (vlen > UINT16_MAX) {
        w->overflow = true;
        return;
    }
    proto_wr16(w->buf + at + 1, (uint16_t)vlen);
}

bool proto_writer_ok(const proto_writer_t *w) { return !w->overflow && w->depth == 0; }
