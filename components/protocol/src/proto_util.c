#include "epager/proto_util.h"

#include "epager/proto_gen.h"

/* ------------------------------------------------------------------ CRC-32 */

/* Nibble table for the reflected polynomial 0xEDB88320: small and fast enough. */
static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t proto_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0F];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0F];
    }
    return ~crc;
}

/* ------------------------------------------------------------------ UTF-8 */

/* Length of the sequence starting with lead byte b, and the allowed range of the
 * second byte (RFC 3629 table 3-7). Returns 0 for an invalid lead byte. */
static unsigned seq_info(uint8_t b, uint8_t *lo, uint8_t *hi)
{
    *lo = 0x80;
    *hi = 0xBF;
    if (b < 0x80) return 1;
    if (b < 0xC2) return 0;
    if (b < 0xE0) return 2;
    if (b < 0xF0) {
        if (b == 0xE0) *lo = 0xA0;
        if (b == 0xED) *hi = 0x9F;
        return 3;
    }
    if (b < 0xF5) {
        if (b == 0xF0) *lo = 0x90;
        if (b == 0xF4) *hi = 0x8F;
        return 4;
    }
    return 0;
}

/* Decode one sequence; *ok = false for invalid input (then *cp = U+FFFD and the
 * return value is the maximal valid prefix, at least 1). */
static size_t decode(const uint8_t *s, size_t len, uint32_t *cp, bool *ok)
{
    uint8_t lo, hi;
    unsigned n = seq_info(s[0], &lo, &hi);
    *ok = false;
    *cp = 0xFFFDu;
    if (n == 0) {
        return 1;
    }
    uint32_t v = (n == 1) ? s[0] : (s[0] & (0xFFu >> (n + 1)));
    for (unsigned i = 1; i < n; i++) {
        uint8_t min = (i == 1) ? lo : 0x80, max = (i == 1) ? hi : 0xBF;
        if (i >= len || s[i] < min || s[i] > max) {
            return i;
        }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    *ok = true;
    *cp = v;
    return n;
}

size_t proto_utf8_decode(const uint8_t *s, size_t len, uint32_t *cp)
{
    bool ok;
    return decode(s, len, cp, &ok);
}

bool proto_utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        bool ok;
        i += decode(s + i, len - i, &cp, &ok);
        if (!ok) {
            return false;
        }
    }
    return true;
}

size_t proto_utf8_encode(uint32_t cp, uint8_t out[4])
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* ------------------------------------------------------------------ text (protocol.md 8) */

/* The generated range tables are sorted and do not overlap (checked by tools/codegen/gen.py). */
static bool in_ranges(uint32_t cp, const proto_cp_range_t *r, size_t n)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].first) {
            hi = mid;
        } else if (cp > r[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

bool proto_text_default_renderable(uint32_t cp, void *ctx)
{
    (void)ctx;
    return in_ranges(cp, proto_renderable_ranges, proto_renderable_ranges_count) ||
           in_ranges(cp, proto_emoji_ranges, proto_emoji_ranges_count);
}

static bool is_mark(uint32_t cp) { return cp >= PROTO_TEXT_MARK_FIRST && cp <= PROTO_TEXT_MARK_LAST; }
static uint8_t ccc(uint32_t mark) { return proto_mark_ccc[mark - PROTO_TEXT_MARK_FIRST]; }

static uint32_t compose_pair(uint32_t base, uint32_t mark)
{
    if (base > 0x10FFFFu) {
        return 0;
    }
    uint32_t key = (base << 10) | (mark - PROTO_TEXT_MARK_FIRST);
    size_t lo = 0, hi = proto_compose_pairs_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (proto_compose_pairs[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < proto_compose_pairs_count && proto_compose_pairs[lo].key == key) ? proto_compose_pairs[lo].composed : 0;
}

#define NO_BASE 0xFFFFFFFFu
#define ELLIPSIS_LEN 3u /* UTF-8 length of PROTO_TEXT_ELLIPSIS (U+2026) */

typedef struct {
    /* emoji sequences (stage A) */
    bool after_zwj;
    bool ri_open;
    /* output (stage D) */
    uint8_t *dst;
    size_t limit;    /* min(budget, dst_cap) */
    size_t len;
    size_t cut;      /* longest prefix length that still leaves room for the ellipsis */
    bool stopped;
    /* render state (stage C) */
    bool in_run;
    bool prev_renderable;
    proto_renderable_fn fn;
    void *ctx;
    /* cluster state (stage B) */
    bool have_cluster;
    uint32_t base;
    uint32_t marks[PROTO_TEXT_MAX_MARKS];
    unsigned n_marks;
} text_state_t;

static void emit(text_state_t *s, uint32_t cp)
{
    if (s->stopped) {
        return;
    }
    uint8_t enc[4];
    size_t n = proto_utf8_encode(cp, enc);
    if (n > s->limit - s->len) {
        /* Overflow: cut back and append the ellipsis if it fits in the limit. */
        uint8_t ell[4];
        size_t en = proto_utf8_encode(PROTO_TEXT_ELLIPSIS, ell);
        if (s->limit >= en) {
            s->len = s->cut;
            for (size_t i = 0; i < en; i++) {
                s->dst[s->len++] = ell[i];
            }
        }
        s->stopped = true;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s->dst[s->len++] = enc[i];
    }
    if (s->limit >= ELLIPSIS_LEN && s->len <= s->limit - ELLIPSIS_LEN) {
        s->cut = s->len;
    }
}

static void render(text_state_t *s, uint32_t cp) /* stage C */
{
    bool mark = is_mark(cp);
    if (mark && s->prev_renderable) {
        return;
    }
    if (!mark && s->fn(cp, s->ctx)) {
        emit(s, cp);
        s->prev_renderable = true;
        s->in_run = false;
        return;
    }
    if (!s->in_run) {
        emit(s, PROTO_TEXT_PLACEHOLDER);
        s->in_run = true;
    }
    s->prev_renderable = false;
}

static void flush_cluster(text_state_t *s) /* stage B: canonical ordering + composition */
{
    if (!s->have_cluster) {
        return;
    }
    s->have_cluster = false;
    uint32_t *m = s->marks;
    unsigned n = s->n_marks;
    if (s->base == NO_BASE) {
        for (unsigned i = 0; i < n; i++) {
            render(s, m[i]);
        }
        return;
    }
    for (unsigned i = 1; i < n; i++) { /* stable insertion sort by combining class */
        uint32_t x = m[i];
        unsigned j = i;
        while (j > 0 && ccc(m[j - 1]) > ccc(x)) {
            m[j] = m[j - 1];
            j--;
        }
        m[j] = x;
    }
    uint32_t base = s->base;
    uint32_t left[PROTO_TEXT_MAX_MARKS];
    unsigned n_left = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = ccc(m[i]);
        bool blocked = false;
        for (unsigned k = 0; k < n_left; k++) {
            uint8_t lc = ccc(left[k]);
            if (lc == 0 || lc >= c) {
                blocked = true;
                break;
            }
        }
        uint32_t composed = blocked ? 0 : compose_pair(base, m[i]);
        if (composed) {
            base = composed;
        } else {
            left[n_left++] = m[i];
        }
    }
    render(s, base);
    for (unsigned i = 0; i < n_left; i++) {
        render(s, left[i]);
    }
}

static void cluster(text_state_t *s, uint32_t cp) /* stage B input */
{
    if (is_mark(cp)) {
        if (!s->have_cluster) {
            s->have_cluster = true;
            s->base = NO_BASE;
            s->n_marks = 0;
        }
        if (s->n_marks < PROTO_TEXT_MAX_MARKS) {
            s->marks[s->n_marks++] = cp;
        }
        return;
    }
    flush_cluster(s);
    s->have_cluster = true;
    s->base = cp;
    s->n_marks = 0;
}

size_t proto_text_normalize(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap, size_t budget,
                            proto_renderable_fn fn, void *ctx)
{
    text_state_t s = {0};
    s.dst = dst;
    s.limit = budget < dst_cap ? budget : dst_cap;
    s.fn = fn ? fn : proto_text_default_renderable;
    s.ctx = ctx;
    size_t i = 0;
    while (i < len && !s.stopped) {
        uint32_t cp;
        i += proto_utf8_decode(src + i, len - i, &cp);
        if (cp == '\r') { /* stage A: controls and format characters */
            if (i < len && src[i] == '\n') {
                continue;
            }
            cp = '\n';
        } else if (cp == '\t') {
            cp = ' ';
        } else if (cp == PROTO_TEXT_ZWJ) {
            s.after_zwj = true; /* a ZWJ sequence shows only its first emoji */
            continue;
        } else if ((cp < 0x20 && cp != '\n') || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
                   in_ranges(cp, proto_removed_ranges, proto_removed_ranges_count)) {
            continue;
        }
        if (s.after_zwj && in_ranges(cp, proto_emoji_ranges, proto_emoji_ranges_count)) {
            s.after_zwj = false;
            continue;
        }
        s.after_zwj = false;
        if (cp >= PROTO_TEXT_RI_FIRST && cp <= PROTO_TEXT_RI_LAST) { /* a flag is two regional indicators */
            if (s.ri_open) {
                s.ri_open = false;
                continue;
            }
            s.ri_open = true;
            cp = PROTO_TEXT_FLAG_GLYPH;
        } else {
            s.ri_open = false;
        }
        cluster(&s, cp);
    }
    flush_cluster(&s);
    return s.len;
}
