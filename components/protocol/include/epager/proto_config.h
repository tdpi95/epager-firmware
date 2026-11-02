/* Config key lookup and per-key value validation (protocol.md 5). Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NULL if unknown. */
const proto_cfg_key_desc_t *proto_cfg_key(uint16_t id);
/* True if the key exists on this variant (PROTO_VARIANT_*). */
bool proto_cfg_key_applies(const proto_cfg_key_desc_t *k, uint8_t variant);
/* Status for writing value to key on a board of the given variant. Order: UNKNOWN_KEY,
 * READ_ONLY, TYPE_MISMATCH / TOO_LARGE, INVALID_UTF8, OUT_OF_RANGE. */
proto_status_t proto_cfg_validate(uint16_t key, const uint8_t *val, size_t len, uint8_t variant);

#ifdef __cplusplus
}
#endif
