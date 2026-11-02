/* SHA-256 (FIPS 180-4), used to verify transfers (protocol.md 7). Portable C11.
 * Software implementation by design (PLAN D24): one code path on host and target. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SHA256_LEN 32u

typedef struct {
    uint32_t h[8];
    uint64_t total; /* bytes hashed so far */
    uint8_t buf[64];
    uint8_t n;      /* bytes pending in buf */
} proto_sha256_t;

void proto_sha256_init(proto_sha256_t *s);
void proto_sha256_update(proto_sha256_t *s, const void *data, size_t len);
void proto_sha256_final(proto_sha256_t *s, uint8_t out[PROTO_SHA256_LEN]);
/* One-shot convenience. */
void proto_sha256(const void *data, size_t len, uint8_t out[PROTO_SHA256_LEN]);

#ifdef __cplusplus
}
#endif
