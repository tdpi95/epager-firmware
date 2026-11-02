/* TLV reader/writer and message header (protocol.md 2.2-2.3). Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_HEADER_LEN 4u
#define PROTO_TLV_HDR_LEN 3u

typedef struct {
    uint8_t tag;
    uint16_t len;
    const uint8_t *val;
} proto_tlv_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} proto_tlv_reader_t;

void proto_tlv_reader_init(proto_tlv_reader_t *r, const uint8_t *buf, size_t len);

/* Returns true and fills *out for the next TLV. Returns false at the end of the body
 * (*err = PROTO_OK) or on a structural error (*err = PROTO_MALFORMED: truncated header,
 * value past the end, or tag 0). */
bool proto_tlv_next(proto_tlv_reader_t *r, proto_tlv_t *out, proto_status_t *err);

/* Writer: errors are sticky; check proto_writer_ok() once at the end. */
#define PROTO_WRITER_MAX_DEPTH 4

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
    uint8_t depth;
    size_t open[PROTO_WRITER_MAX_DEPTH]; /* offsets of open struct TLV headers */
} proto_writer_t;

void proto_writer_init(proto_writer_t *w, uint8_t *buf, size_t cap);
void proto_w_header(proto_writer_t *w, uint8_t type, uint16_t req_id);
void proto_w_u8(proto_writer_t *w, uint8_t tag, uint8_t v);
void proto_w_u16(proto_writer_t *w, uint8_t tag, uint16_t v);
void proto_w_u32(proto_writer_t *w, uint8_t tag, uint32_t v);
void proto_w_i16(proto_writer_t *w, uint8_t tag, int16_t v);
void proto_w_i32(proto_writer_t *w, uint8_t tag, int32_t v);
void proto_w_bool(proto_writer_t *w, uint8_t tag, bool v);
void proto_w_bytes(proto_writer_t *w, uint8_t tag, const void *data, size_t len);
void proto_w_str(proto_writer_t *w, uint8_t tag, const char *s); /* NUL-terminated */
void proto_w_struct_begin(proto_writer_t *w, uint8_t tag);
void proto_w_struct_end(proto_writer_t *w);
/* True if nothing overflowed and every struct was closed. */
bool proto_writer_ok(const proto_writer_t *w);

/* Little-endian helpers */
static inline uint16_t proto_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t proto_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void proto_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void proto_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#ifdef __cplusplus
}
#endif
