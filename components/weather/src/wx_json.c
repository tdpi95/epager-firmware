/* Bounded JSON reader: finds one value by key path without building a tree. Every step checks the
 * end of the buffer; nesting is limited to WX_JSON_DEPTH. */
#include <string.h>

#include "epager/proto_util.h"
#include "epager/wx.h"

typedef struct {
    const char *p, *end;
} cur_t;

static void ws(cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static bool skip_string(cur_t *c)
{
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    for (c->p++; c->p < c->end; c->p++) {
        if (*c->p == '\\') {
            c->p++; /* the escaped byte; \uXXXX digits are ordinary bytes */
        } else if (*c->p == '"') {
            c->p++;
            return true;
        }
    }
    return false;
}

static bool skip_value(cur_t *c, int depth);

/* At '{' or '[': skip the whole container. */
static bool skip_container(cur_t *c, int depth)
{
    if (depth >= WX_JSON_DEPTH) {
        return false;
    }
    const char close = *c->p == '{' ? '}' : ']';
    const bool object = close == '}';
    c->p++;
    ws(c);
    if (c->p < c->end && *c->p == close) {
        c->p++;
        return true;
    }
    for (;;) {
        if (object) {
            ws(c);
            if (!skip_string(c)) {
                return false;
            }
            ws(c);
            if (c->p >= c->end || *c->p != ':') {
                return false;
            }
            c->p++;
        }
        if (!skip_value(c, depth + 1)) {
            return false;
        }
        ws(c);
        if (c->p >= c->end) {
            return false;
        }
        if (*c->p == close) {
            c->p++;
            return true;
        }
        if (*c->p != ',') {
            return false;
        }
        c->p++;
    }
}

static bool skip_value(cur_t *c, int depth)
{
    ws(c);
    if (c->p >= c->end) {
        return false;
    }
    char ch = *c->p;
    if (ch == '"') {
        return skip_string(c);
    }
    if (ch == '{' || ch == '[') {
        return skip_container(c, depth);
    }
    const char *start = c->p; /* number or literal */
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || (*c->p >= 'a' && *c->p <= 'z') || *c->p == '-' ||
                             *c->p == '+' || *c->p == '.' || *c->p == 'E')) {
        c->p++;
    }
    return c->p > start;
}

/* At '{': move to the value of member `key`. */
static bool find_member(cur_t *c, const char *key, size_t klen)
{
    c->p++;
    for (;;) {
        ws(c);
        if (c->p >= c->end || *c->p != '"') {
            return false; /* '}' (not found) or malformed */
        }
        const char *k = c->p + 1;
        if (!skip_string(c)) {
            return false;
        }
        bool match = (size_t)(c->p - 1 - k) == klen && memcmp(k, key, klen) == 0;
        ws(c);
        if (c->p >= c->end || *c->p != ':') {
            return false;
        }
        c->p++;
        ws(c);
        if (match) {
            return true;
        }
        if (!skip_value(c, 1)) {
            return false;
        }
        ws(c);
        if (c->p >= c->end || *c->p != ',') {
            return false;
        }
        c->p++;
    }
}

/* At '[': move to element `idx`. */
static bool find_element(cur_t *c, unsigned long idx)
{
    c->p++;
    ws(c);
    if (c->p < c->end && *c->p == ']') {
        return false;
    }
    for (unsigned long i = 0;; i++) {
        ws(c);
        if (i == idx) {
            return c->p < c->end;
        }
        if (!skip_value(c, 1)) {
            return false;
        }
        ws(c);
        if (c->p >= c->end || *c->p != ',') {
            return false;
        }
        c->p++;
    }
}

bool wx_json_get(const char *doc, size_t len, const char *path, wx_span_t *out)
{
    cur_t c = {doc, doc + len};
    ws(&c);
    const char *s = path;
    while (*s) {
        if (c.p >= c.end) {
            return false;
        }
        if (*s == '[') {
            unsigned long idx = 0;
            for (s++; *s >= '0' && *s <= '9'; s++) {
                idx = idx * 10u + (unsigned long)(*s - '0');
            }
            if (*s++ != ']' || *c.p != '[' || !find_element(&c, idx)) {
                return false;
            }
        } else {
            if (*s == '.') {
                s++;
            }
            size_t klen = strcspn(s, ".[");
            if (klen == 0 || *c.p != '{' || !find_member(&c, s, klen)) {
                return false;
            }
            s += klen;
        }
    }
    ws(&c);
    cur_t v = c;
    if (!skip_value(&v, 0)) {
        return false;
    }
    out->p = c.p;
    out->n = (size_t)(v.p - c.p);
    return true;
}

bool wx_json_fixed(wx_span_t v, unsigned decimals, int32_t *out)
{
    const char *p = v.p, *end = v.p + v.n;
    bool neg = p < end && *p == '-';
    p += neg;
    int64_t acc = 0;
    int int_digits = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++) {
        if (++int_digits > 9) {
            return false;
        }
        acc = acc * 10 + (*p - '0');
    }
    if (int_digits == 0) {
        return false;
    }
    unsigned frac = 0;
    bool round_up = false;
    if (p < end && *p == '.') {
        p++;
        if (p >= end || *p < '0' || *p > '9') {
            return false;
        }
        for (; p < end && *p >= '0' && *p <= '9'; p++) {
            if (frac < decimals) {
                acc = acc * 10 + (*p - '0');
                frac++;
            } else if (frac == decimals) {
                round_up = *p >= '5';
                frac++;
            }
        }
    }
    if (p != end) {
        return false; /* exponent or junk */
    }
    for (; frac < decimals; frac++) {
        acc *= 10;
    }
    acc += round_up;
    if (neg) {
        acc = -acc;
    }
    if (acc > INT32_MAX || acc < INT32_MIN) {
        return false;
    }
    *out = (int32_t)acc;
    return true;
}

static int hex4(const char *p, const char *end)
{
    if (end - p < 4) {
        return -1;
    }
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

bool wx_json_string(wx_span_t v, char *out, size_t cap, size_t *len)
{
    if (v.n < 2 || v.p[0] != '"' || v.p[v.n - 1] != '"' || cap == 0) {
        return false;
    }
    const char *p = v.p + 1, *end = v.p + v.n - 1;
    size_t n = 0;
    bool full = false;
    while (p < end) {
        uint8_t buf[4];
        size_t bl;
        if (*p != '\\') {
            buf[0] = (uint8_t)*p++;
            bl = 1;
        } else {
            if (++p >= end) {
                return false;
            }
            char e = *p++;
            uint32_t cp;
            switch (e) {
            case '"': case '\\': case '/': cp = (uint8_t)e; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u': {
                int h = hex4(p, end);
                if (h < 0) {
                    return false;
                }
                p += 4;
                cp = (uint32_t)h;
                if (cp >= 0xD800 && cp <= 0xDBFF) { /* a surrogate pair */
                    int lo = end - p >= 6 && p[0] == '\\' && p[1] == 'u' ? hex4(p + 2, end) : -1;
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        return false;
                    }
                    p += 6;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                } else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0) {
                    return false; /* a lone low surrogate, or a NUL that would end the C string */
                }
                break;
            }
            default: return false;
            }
            bl = proto_utf8_encode(cp, buf);
        }
        /* a raw multi-byte character arrives byte by byte: if one does not fit, what was copied of it
         * is dropped, so the cut never splits a character */
        if (bl == 1 && (buf[0] & 0xC0u) == 0x80u) {
            if (!full) {
                if (n + 1 >= cap) {
                    full = true; /* a continuation that does not fit: drop the whole character */
                    while (n > 0 && ((uint8_t)out[n - 1] & 0xC0u) == 0x80u) {
                        n--;
                    }
                    if (n > 0 && (uint8_t)out[n - 1] >= 0xC0u) {
                        n--;
                    }
                } else {
                    out[n++] = (char)buf[0];
                }
            }
            continue;
        }
        if (!full && n + bl < cap) {
            memcpy(out + n, buf, bl);
            n += bl;
        } else {
            full = true;
        }
    }
    out[n] = '\0';
    if (!proto_utf8_valid((const uint8_t *)out, n)) {
        return false;
    }
    if (len) {
        *len = n;
    }
    return true;
}
