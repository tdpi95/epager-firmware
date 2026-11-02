#include "epager/proto_config.h"

#include "epager/proto_tlv.h"
#include "epager/proto_util.h"

const proto_cfg_key_desc_t *proto_cfg_key(uint16_t id)
{
    for (size_t i = 0; i < proto_cfg_keys_count; i++) {
        if (proto_cfg_keys[i].id == id) {
            return &proto_cfg_keys[i];
        }
    }
    return NULL;
}

bool proto_cfg_key_applies(const proto_cfg_key_desc_t *k, uint8_t variant)
{
    return k->variant_mask == 0 || (variant < 32 && (k->variant_mask & (1u << variant)));
}

proto_status_t proto_cfg_validate(uint16_t key, const uint8_t *val, size_t len, uint8_t variant)
{
    const proto_cfg_key_desc_t *k = proto_cfg_key(key);
    if (!k || !proto_cfg_key_applies(k, variant)) {
        return PROTO_UNKNOWN_KEY;
    }
    if (k->flags & PROTO_CFG_READ_ONLY) {
        return PROTO_READ_ONLY;
    }
    int64_t x;
    switch (k->type) {
    case PROTO_FT_BOOL:
        if (len != 1) return PROTO_TYPE_MISMATCH;
        return val[0] <= 1 ? PROTO_OK : PROTO_OUT_OF_RANGE;
    case PROTO_FT_STR:
        if (len > k->max_len) return PROTO_TOO_LARGE;
        return proto_utf8_valid(val, len) ? PROTO_OK : PROTO_INVALID_UTF8;
    case PROTO_FT_U8:
        if (len != 1) return PROTO_TYPE_MISMATCH;
        x = val[0];
        break;
    case PROTO_FT_U16:
        if (len != 2) return PROTO_TYPE_MISMATCH;
        x = proto_rd16(val);
        break;
    case PROTO_FT_U32:
        if (len != 4) return PROTO_TYPE_MISMATCH;
        x = proto_rd32(val);
        break;
    case PROTO_FT_I16:
        if (len != 2) return PROTO_TYPE_MISMATCH;
        x = (int16_t)proto_rd16(val);
        break;
    case PROTO_FT_I32:
        if (len != 4) return PROTO_TYPE_MISMATCH;
        x = (int32_t)proto_rd32(val);
        break;
    default:
        return PROTO_INTERNAL;
    }
    if (((k->flags & PROTO_CFG_HAS_MIN) && x < k->min) || ((k->flags & PROTO_CFG_HAS_MAX) && x > k->max)) {
        return PROTO_OUT_OF_RANGE;
    }
    if (k->enum_values) {
        for (uint8_t i = 0; i < k->n_enum_values; i++) {
            if (k->enum_values[i] == x) {
                return PROTO_OK;
            }
        }
        return PROTO_OUT_OF_RANGE;
    }
    return PROTO_OK;
}
