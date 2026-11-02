/*
 * Descriptor types for the table-driven message codec. The tables themselves are
 * generated from contracts/registry.json into proto_gen.h / proto_gen.c.
 * Portable C11: no ESP-IDF headers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTO_FT_U8,
    PROTO_FT_U16,
    PROTO_FT_U32,
    PROTO_FT_I16,
    PROTO_FT_I32,
    PROTO_FT_BOOL,
    PROTO_FT_STR,   /* strict UTF-8 */
    PROTO_FT_ID,    /* [a-z0-9._-], 1..32 bytes, not starting with '.' */
    PROTO_FT_TEXT,  /* UTF-8 not validated on the wire; sanitised for display */
    PROTO_FT_BYTES,
    PROTO_FT_STRUCT,
} proto_ftype_t;

#define PROTO_F_REQUIRED 0x01u
#define PROTO_F_REPEATED 0x02u
#define PROTO_F_HAS_MIN  0x04u
#define PROTO_F_HAS_MAX  0x08u

struct proto_struct_desc;

typedef struct {
    const char *name;
    uint8_t tag;
    uint8_t type;      /* proto_ftype_t */
    uint8_t flags;     /* PROTO_F_* */
    uint16_t min_len;  /* variable-length types; 0 = no minimum */
    uint16_t max_len;  /* variable-length types; 0 = unlimited */
    int32_t min;       /* integers, valid with PROTO_F_HAS_MIN */
    int32_t max;       /* integers, valid with PROTO_F_HAS_MAX */
    const struct proto_struct_desc *nested; /* PROTO_FT_STRUCT */
} proto_field_desc_t;

typedef struct proto_struct_desc {
    const char *name;
    const proto_field_desc_t *fields; /* sorted by tag */
    uint8_t n_fields;
} proto_struct_desc_t;

typedef enum {
    PROTO_KIND_REQUEST,
    PROTO_KIND_RESPONSE,
    PROTO_KIND_EVENT,
    PROTO_KIND_READ,
    PROTO_KIND_ERROR,
} proto_kind_t;

typedef enum {
    PROTO_CHAN_ANY,
    PROTO_CHAN_CAPABILITIES,
    PROTO_CHAN_CONFIG,
    PROTO_CHAN_NOTIFICATION,
    PROTO_CHAN_TRANSFER,
    PROTO_CHAN_STATUS,
} proto_channel_t;

typedef struct {
    uint8_t id;
    uint8_t kind;    /* proto_kind_t */
    uint8_t channel; /* proto_channel_t */
    proto_struct_desc_t body;
} proto_msg_desc_t;

/* Config keys */
#define PROTO_CFG_READ_ONLY 0x01u
#define PROTO_CFG_HAS_MIN   0x02u
#define PROTO_CFG_HAS_MAX   0x04u

typedef struct {
    const char *name;
    uint16_t id;
    uint8_t type;            /* proto_ftype_t: U8..I32, BOOL or STR */
    uint8_t flags;           /* PROTO_CFG_* */
    uint8_t max_len;         /* STR */
    int32_t min, max;
    uint32_t variant_mask;   /* bit n = variant enum value n; 0 = all variants */
    const uint8_t *enum_values; /* NULL = not an enum */
    uint8_t n_enum_values;
    int32_t default_int;     /* integer/bool keys */
    const char *default_str; /* STR keys */
    uint32_t since;          /* contract version that added the key, packed (PROTO_CONTRACT_VERSION_PACKED) */
} proto_cfg_key_desc_t;

#ifdef __cplusplus
}
#endif
