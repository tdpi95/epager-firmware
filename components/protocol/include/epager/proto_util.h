/* CRC-32, UTF-8 and board text normalisation (protocol.md 7-8). Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 IEEE 802.3 / zlib. Start with crc = 0; crc32(a+b) == update(update(0, a), b). */
uint32_t proto_crc32_update(uint32_t crc, const void *data, size_t len);

/* Strict UTF-8 (RFC 3629: no overlongs, surrogates or code points above U+10FFFF). */
bool proto_utf8_valid(const uint8_t *s, size_t len);
/* Lenient decode of one code point at s (len > 0). Invalid input yields U+FFFD and
 * consumes the maximal valid prefix (at least 1 byte). Returns bytes consumed. */
size_t proto_utf8_decode(const uint8_t *s, size_t len, uint32_t *cp);
/* Encode cp (<= U+10FFFF, not a surrogate) into out[4]. Returns length. */
size_t proto_utf8_encode(uint32_t cp, uint8_t out[4]);

/* Font coverage predicate. */
typedef bool (*proto_renderable_fn)(uint32_t cp, void *ctx);
/* Reference set: registry text.renderable_ranges plus text.emoji_ranges. */
bool proto_text_default_renderable(uint32_t cp, void *ctx);

#define PROTO_TEXT_NO_BUDGET ((size_t)-1)

/* Normalise src for display (protocol.md 8) into dst. The output is limited to
 * min(budget, dst_cap) bytes with the same truncation rule (ellipsis appended when cut).
 * Output is valid UTF-8, not NUL-terminated. Returns the output length.
 * fn == NULL uses proto_text_default_renderable. */
size_t proto_text_normalize(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap, size_t budget,
                            proto_renderable_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
