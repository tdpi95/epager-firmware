#include "epager/proto_msg.h"

#include <string.h>

#include "epager/proto_util.h"

#define MAX_NESTING 4

const proto_msg_desc_t *proto_msg_desc(uint8_t type)
{
    for (size_t i = 0; i < proto_messages_count; i++) {
        if (proto_messages[i].id == type) {
            return &proto_messages[i];
        }
    }
    return NULL;
}

const proto_msg_desc_t *proto_msg_desc_by_name(const char *name)
{
    for (size_t i = 0; i < proto_messages_count; i++) {
        if (strcmp(proto_messages[i].body.name, name) == 0) {
            return &proto_messages[i];
        }
    }
    return NULL;
}

const char *proto_status_name(proto_status_t st)
{
    if ((unsigned)st < PROTO_STATUS_COUNT && proto_status_names[st]) {
        return proto_status_names[st];
    }
    return "UNKNOWN";
}

static const proto_field_desc_t *field_by_tag(const proto_struct_desc_t *d, uint8_t tag, unsigned *index)
{
    for (unsigned i = 0; i < d->n_fields; i++) {
        if (d->fields[i].tag == tag) {
            *index = i;
            return &d->fields[i];
        }
    }
    return NULL;
}

static bool valid_id(const uint8_t *v, size_t n)
{
    if (v[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = v[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static proto_status_t validate(const proto_struct_desc_t *d, const uint8_t *body, size_t len, int depth);

static int int_width(uint8_t type)
{
    switch (type) {
    case PROTO_FT_U8: return 1;
    case PROTO_FT_U16: case PROTO_FT_I16: return 2;
    case PROTO_FT_U32: case PROTO_FT_I32: return 4;
    default: return 0;
    }
}

static proto_status_t validate_value(const proto_field_desc_t *f, const uint8_t *v, uint16_t n, int depth)
{
    int w = int_width(f->type);
    if (w) {
        if (n != w) {
            return PROTO_MALFORMED;
        }
        int64_t x;
        switch (f->type) {
        case PROTO_FT_U8: x = v[0]; break;
        case PROTO_FT_U16: x = proto_rd16(v); break;
        case PROTO_FT_U32: x = proto_rd32(v); break;
        case PROTO_FT_I16: x = (int16_t)proto_rd16(v); break;
        default: x = (int32_t)proto_rd32(v); break;
        }
        if (((f->flags & PROTO_F_HAS_MIN) && x < f->min) || ((f->flags & PROTO_F_HAS_MAX) && x > f->max)) {
            return PROTO_OUT_OF_RANGE;
        }
        return PROTO_OK;
    }
    if (f->type == PROTO_FT_BOOL) {
        return (n == 1 && v[0] <= 1) ? PROTO_OK : PROTO_MALFORMED;
    }
    if (f->type == PROTO_FT_STRUCT) {
        return validate(f->nested, v, n, depth + 1);
    }
    if (f->max_len && n > f->max_len) {
        return PROTO_TOO_LARGE;
    }
    if (n < f->min_len) {
        return PROTO_MALFORMED;
    }
    if (f->type == PROTO_FT_STR || f->type == PROTO_FT_ID) {
        if (!proto_utf8_valid(v, n)) {
            return PROTO_INVALID_UTF8;
        }
        if (f->type == PROTO_FT_ID && !valid_id(v, n)) {
            return PROTO_INVALID_ID;
        }
    }
    return PROTO_OK;
}

static proto_status_t validate(const proto_struct_desc_t *d, const uint8_t *body, size_t len, int depth)
{
    if (depth > MAX_NESTING || d->n_fields > 32) {
        return PROTO_MALFORMED;
    }
    uint32_t seen = 0;
    proto_tlv_reader_t r;
    proto_tlv_t t;
    proto_status_t err;
    proto_tlv_reader_init(&r, body, len);
    while (proto_tlv_next(&r, &t, &err)) {
        unsigned idx;
        const proto_field_desc_t *f = field_by_tag(d, t.tag, &idx);
        if (!f) {
            continue; /* unknown tags are skipped */
        }
        if (!(f->flags & PROTO_F_REPEATED) && (seen & (1u << idx))) {
            return PROTO_MALFORMED;
        }
        seen |= 1u << idx;
        proto_status_t st = validate_value(f, t.val, t.len, depth);
        if (st != PROTO_OK) {
            return st;
        }
    }
    if (err != PROTO_OK) {
        return err;
    }
    for (unsigned i = 0; i < d->n_fields; i++) { /* fields are sorted by tag */
        if ((d->fields[i].flags & PROTO_F_REQUIRED) && !(seen & (1u << i))) {
            return PROTO_MISSING_FIELD;
        }
    }
    return PROTO_OK;
}

proto_status_t proto_validate_body(const proto_struct_desc_t *desc, const uint8_t *body, size_t len)
{
    return validate(desc, body, len, 0);
}

proto_status_t proto_msg_parse(const uint8_t *buf, size_t len, proto_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < PROTO_HEADER_LEN) {
        return PROTO_MALFORMED;
    }
    out->header_valid = true;
    out->ver = buf[0];
    out->type = buf[1];
    out->req_id = proto_rd16(buf + 2);
    out->body = buf + PROTO_HEADER_LEN;
    out->body_len = len - PROTO_HEADER_LEN;
    if (out->ver != PROTO_MAJOR) {
        return PROTO_UNSUPPORTED_VERSION;
    }
    out->desc = proto_msg_desc(out->type);
    if (!out->desc) {
        return PROTO_UNKNOWN_MESSAGE;
    }
    return validate(&out->desc->body, out->body, out->body_len, 0);
}

bool proto_find_next(const uint8_t *body, size_t len, uint8_t tag, size_t *pos, const uint8_t **val, uint16_t *vlen)
{
    proto_tlv_reader_t r;
    proto_tlv_t t;
    proto_status_t err;
    proto_tlv_reader_init(&r, body, len);
    r.pos = *pos;
    while (proto_tlv_next(&r, &t, &err)) {
        if (t.tag == tag) {
            *val = t.val;
            *vlen = t.len;
            *pos = r.pos;
            return true;
        }
    }
    *pos = len;
    return false;
}

bool proto_find(const uint8_t *body, size_t len, uint8_t tag, const uint8_t **val, uint16_t *vlen)
{
    size_t pos = 0;
    return proto_find_next(body, len, tag, &pos, val, vlen);
}

static const uint8_t *find_fixed(const uint8_t *body, size_t len, uint8_t tag, uint16_t width)
{
    const uint8_t *v;
    uint16_t n;
    return (proto_find(body, len, tag, &v, &n) && n == width) ? v : NULL;
}

bool proto_get_u8(const uint8_t *body, size_t len, uint8_t tag, uint8_t *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 1);
    if (v) { *out = v[0]; }
    return v != NULL;
}

bool proto_get_u16(const uint8_t *body, size_t len, uint8_t tag, uint16_t *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 2);
    if (v) { *out = proto_rd16(v); }
    return v != NULL;
}

bool proto_get_u32(const uint8_t *body, size_t len, uint8_t tag, uint32_t *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 4);
    if (v) { *out = proto_rd32(v); }
    return v != NULL;
}

bool proto_get_i16(const uint8_t *body, size_t len, uint8_t tag, int16_t *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 2);
    if (v) { *out = (int16_t)proto_rd16(v); }
    return v != NULL;
}

bool proto_get_i32(const uint8_t *body, size_t len, uint8_t tag, int32_t *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 4);
    if (v) { *out = (int32_t)proto_rd32(v); }
    return v != NULL;
}

bool proto_get_bool(const uint8_t *body, size_t len, uint8_t tag, bool *out)
{
    const uint8_t *v = find_fixed(body, len, tag, 1);
    if (v) { *out = v[0] != 0; }
    return v != NULL;
}

void proto_w_response(proto_writer_t *w, const proto_msg_t *req, proto_status_t status)
{
    proto_w_header(w, proto_response_type(req->type), req->req_id);
    proto_w_u8(w, 1, (uint8_t)status);
}
