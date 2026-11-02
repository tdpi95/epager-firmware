/* Message parsing and validation (protocol.md 2.2-2.4). Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"
#include "epager/proto_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool header_valid;   /* at least 4 bytes: ver/type/req_id are meaningful */
    uint8_t ver;
    uint8_t type;
    uint16_t req_id;
    const proto_msg_desc_t *desc; /* NULL if the type is unknown */
    const uint8_t *body;
    size_t body_len;
} proto_msg_t;

const proto_msg_desc_t *proto_msg_desc(uint8_t type);
const proto_msg_desc_t *proto_msg_desc_by_name(const char *name);
const char *proto_status_name(proto_status_t st); /* "UNKNOWN" for codes not in the registry */

/* Parse the header and fully validate the body in the order of protocol.md 2.4.
 * Even on error, *out has header_valid/type/req_id filled when available so the caller
 * can answer (ERROR, or the request's response type with only a status). */
proto_status_t proto_msg_parse(const uint8_t *buf, size_t len, proto_msg_t *out);

/* Validate a body (or nested struct body) against a descriptor. */
proto_status_t proto_validate_body(const proto_struct_desc_t *desc, const uint8_t *body, size_t len);

/* Accessors for validated bodies. Lookups are top-level within the given body; to read a
 * struct, pass its value as the body. */
bool proto_find(const uint8_t *body, size_t len, uint8_t tag, const uint8_t **val, uint16_t *vlen);
/* Iterate occurrences of a (repeated) tag: start with *pos = 0. */
bool proto_find_next(const uint8_t *body, size_t len, uint8_t tag, size_t *pos, const uint8_t **val, uint16_t *vlen);
bool proto_get_u8(const uint8_t *body, size_t len, uint8_t tag, uint8_t *out);
bool proto_get_u16(const uint8_t *body, size_t len, uint8_t tag, uint16_t *out);
bool proto_get_u32(const uint8_t *body, size_t len, uint8_t tag, uint32_t *out);
bool proto_get_i16(const uint8_t *body, size_t len, uint8_t tag, int16_t *out);
bool proto_get_i32(const uint8_t *body, size_t len, uint8_t tag, int32_t *out);
bool proto_get_bool(const uint8_t *body, size_t len, uint8_t tag, bool *out);

static inline uint8_t proto_response_type(uint8_t request_type) { return (uint8_t)(request_type | 0x80u); }

/* Start the response to req: header (response type, echoed req_id) and the status field (tag 1).
 * A failed request's response is complete after this call (protocol.md 2.4). */
void proto_w_response(proto_writer_t *w, const proto_msg_t *req, proto_status_t status);

#ifdef __cplusplus
}
#endif
