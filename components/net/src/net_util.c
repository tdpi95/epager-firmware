/* Portable helpers of the Wi-Fi task: setup hotspot and HTTP Date header (net_util.h). */
#include "epager/net_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "epager/board_util.h"
#include "epager/proto_util.h"

/* ---------------------------------------------------------------- DNS (RFC 1035) */

#define DNS_HEADER 12u
#define DNS_TYPE_A 1u
#define DNS_CLASS_IN 1u
#define DNS_TTL_S 60u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t net_dns_answer(const uint8_t *q, size_t n, const uint8_t ip[4], uint8_t *out, size_t cap)
{
    if (n < DNS_HEADER || n > NET_DNS_MAX) {
        return 0;
    }
    const uint16_t flags = rd16(q + 2);
    /* a query (QR 0), standard (opcode 0), one question, no answers */
    if ((flags & 0x8000u) || (flags & 0x7800u) || rd16(q + 4) != 1 || rd16(q + 6) != 0) {
        return 0;
    }
    /* the question's name: labels of 1..63 bytes, at most 255 bytes, no compression pointers */
    size_t pos = DNS_HEADER, name_len = 0;
    for (;;) {
        if (pos >= n) {
            return 0;
        }
        const uint8_t l = q[pos++];
        if (l == 0) {
            break;
        }
        if (l > 63 || pos + l > n || (name_len += l + 1u) > 255) {
            return 0;
        }
        pos += l;
    }
    if (pos + 4 > n) {
        return 0;
    }
    const uint16_t qtype = rd16(q + pos), qclass = rd16(q + pos + 2);
    const size_t question_end = pos + 4;
    const bool answer = qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN;
    const size_t len = question_end + (answer ? 16u : 0u);
    if (len > cap) {
        return 0;
    }
    memcpy(out, q, question_end); /* id, flags, counts and the question; extra records are dropped */
    wr16(out + 2, (uint16_t)(0x8400u | (flags & 0x0100u))); /* QR, AA, RD as asked, RA 0, NOERROR */
    wr16(out + 6, answer ? 1u : 0u);                         /* ANCOUNT */
    wr16(out + 8, 0);                                        /* NSCOUNT */
    wr16(out + 10, 0);                                       /* ARCOUNT */
    if (answer) {
        uint8_t *a = out + question_end;
        wr16(a, 0xC000u | DNS_HEADER); /* the name: a pointer to the question's */
        wr16(a + 2, DNS_TYPE_A);
        wr16(a + 4, DNS_CLASS_IN);
        wr16(a + 6, 0);
        wr16(a + 8, DNS_TTL_S);
        wr16(a + 10, 4);
        memcpy(a + 12, ip, 4);
    }
    return len;
}

/* ---------------------------------------------------------------- form fields */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool net_form_get(const char *body, size_t len, const char *name, char *out, size_t cap)
{
    const size_t nlen = strlen(name);
    size_t pos = 0;
    while (pos < len) {
        const char *amp = memchr(body + pos, '&', len - pos);
        const size_t end = amp ? (size_t)(amp - body) : len;
        if (end - pos > nlen && memcmp(body + pos, name, nlen) == 0 && body[pos + nlen] == '=') {
            size_t o = 0;
            for (size_t i = pos + nlen + 1; i < end; i++) {
                int c = (unsigned char)body[i];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    const int hi = i + 2 < end ? hexval(body[i + 1]) : -1, lo = hi >= 0 ? hexval(body[i + 2]) : -1;
                    if (lo < 0) {
                        return false;
                    }
                    c = hi << 4 | lo;
                    i += 2;
                }
                if (c < 0x20 || c == 0x7F || o + 1 >= cap) {
                    return false; /* control characters (a NUL included), or too long */
                }
                out[o++] = (char)c;
            }
            out[o] = '\0';
            return proto_utf8_valid((const uint8_t *)out, o);
        }
        pos = end + 1;
    }
    return false;
}

/* ---------------------------------------------------------------- HTML */

size_t net_html_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0) {
        return 0;
    }
    for (const char *p = in; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            const size_t l = strlen(rep);
            if (n + l >= cap) {
                break;
            }
            memcpy(out + n, rep, l);
            n += l;
            continue;
        }
        /* a whole character; invalid UTF-8 (a scanned network name can hold any bytes) becomes U+FFFD */
        uint32_t cp;
        const size_t used = proto_utf8_decode((const uint8_t *)p, strlen(p), &cp);
        uint8_t enc[4];
        const size_t l = proto_utf8_encode(cp, enc);
        if (n + l >= cap) {
            break;
        }
        memcpy(out + n, enc, l);
        n += l;
        p += used - 1;
    }
    out[n] = '\0';
    return n;
}

/* ---------------------------------------------------------------- pages */

typedef struct {
    char *out;
    size_t cap, n;
    bool ok;
} page_t;

static void put(page_t *pg, const char *fmt, ...)
{
    if (!pg->ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(pg->out + pg->n, pg->cap - pg->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= pg->cap - pg->n) {
        pg->ok = false;
        return;
    }
    pg->n += (size_t)k;
}

/* Escaped text; the escaped form of a 64-byte value is at most 6 x 64 bytes. */
static void put_text(page_t *pg, const char *s)
{
    char esc[6 * 64 + 1];
    net_html_escape(s, esc, sizeof(esc));
    put(pg, "%s", esc);
}

static const char HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>";
static const char STYLE[] =
    "</title><style>body{font-family:sans-serif;max-width:26em;margin:1em auto;padding:0 1em;color:#1d1f23}"
    "label{display:block;margin-top:.8em}input,button{width:100%;font-size:1.1em;padding:.5em;margin-top:.3em;"
    "box-sizing:border-box}button{margin-top:1.2em;background:#2563eb;color:#fff;border:0;border-radius:2em}"
    "small{color:#6b7280}.n{display:flex;justify-content:space-between;margin-top:.4em;background:#eef1f5;"
    "color:#1d1f23;border-radius:.5em;text-align:left}.n b{font-weight:normal;overflow-wrap:anywhere}"
    ".n span{margin-left:.5em;white-space:nowrap;color:#6b7280}.r input{width:auto;margin:0 .5em 0 0}"
    "</style></head><body>";

/* Signal bars for a scan result's RSSI (dBm), as phones show them. */
static const char *bars(int8_t rssi)
{
    return rssi >= -55 ? "▂▄▆█" : rssi >= -67 ? "▂▄▆" : rssi >= -78 ? "▂▄" : "▂";
}

static const char *const PAGE_EN[] = {
    "Wi-Fi network", "Password (empty for an open network)",
    "City for the weather (empty: found from your internet address)", "Save",
    "Weather data by Open-Meteo.com (CC BY 4.0). The city, or your internet address, is sent to "
    "Open-Meteo and ipinfo.io to find the weather.",
    "Saved", "is joining", "Its screen shows whether it worked. If it did not, join the hotspot again and "
    "open this page.",
    "Networks nearby: tap one to choose it", "Show password",
};
static const char *const PAGE_VI[] = {
    "Mạng Wi-Fi", "Mật khẩu (để trống nếu mạng không có mật khẩu)",
    "Thành phố để xem thời tiết (để trống: tìm theo địa chỉ internet)", "Lưu",
    "Dữ liệu thời tiết của Open-Meteo.com (CC BY 4.0). Tên thành phố, hoặc địa chỉ internet, được gửi tới "
    "Open-Meteo và ipinfo.io để tìm thời tiết.",
    "Đã lưu", "đang kết nối", "Màn hình của thiết bị sẽ cho biết kết quả. Nếu không được, hãy kết nối lại "
    "điểm phát Wi-Fi và mở lại trang này.",
    "Mạng xung quanh: chạm để chọn", "Hiện mật khẩu",
};

size_t net_setup_page(char *out, size_t cap, const net_page_t *p)
{
    const char *const *t = p->vietnamese ? PAGE_VI : PAGE_EN;
    page_t pg = {out, cap, 0, cap > 0};
    put(&pg, "%s", HEAD);
    put_text(&pg, p->device);
    put(&pg, "%s<h1>", STYLE);
    put_text(&pg, p->device);
    put(&pg, "</h1>");
    if (p->note && p->note[0]) {
        put(&pg, "<p><b>");
        put_text(&pg, p->note);
        put(&pg, "</b></p>");
    }
    /* No <script> element: one inline handler on the form copies a tapped network's name (the text of its
     * <b>, decoded by the browser, so a name is never part of the script) and another shows the password. */
    put(&pg, "<form method=post action=/save onclick=\"var b=event.target.closest('.n');"
             "if(b){this.ssid.value=b.firstChild.textContent;this.pw.focus()}\">"
             "<label>%s<input name=ssid maxlength=32 required value=\"", t[0]);
    put_text(&pg, p->ssid ? p->ssid : "");
    put(&pg, "\"></label>");
    if (p->n_aps > 0) {
        put(&pg, "<p><small>%s</small></p>", t[8]);
    }
    for (size_t i = 0; i < p->n_aps && i < NET_APS_MAX; i++) {
        put(&pg, "<button type=button class=n><b>");
        put_text(&pg, p->aps[i].ssid);
        put(&pg, "</b><span>%s%s</span></button>", bars(p->aps[i].rssi), p->aps[i].open ? "" : " 🔒");
    }
    put(&pg, "<label>%s<input name=pw type=password maxlength=63></label>"
             "<label class=r><input type=checkbox onchange=\"this.form.pw.type=this.checked?'text':'password'\">%s"
             "</label>", t[1], t[9]);
    put(&pg, "<label>%s<input name=city maxlength=64 value=\"", t[2]);
    put_text(&pg, p->city ? p->city : "");
    put(&pg, "\"></label><button>%s</button></form><p><small>%s</small></p></body></html>", t[3], t[4]);
    return pg.ok ? pg.n : 0;
}

size_t net_saved_page(char *out, size_t cap, bool vietnamese, const char *ssid)
{
    const char *const *t = vietnamese ? PAGE_VI : PAGE_EN;
    page_t pg = {out, cap, 0, cap > 0};
    put(&pg, "%sePager%s<h1>%s</h1><p>ePager %s <b>", HEAD, STYLE, t[5], t[6]);
    put_text(&pg, ssid);
    put(&pg, "</b>.</p><p>%s</p></body></html>", t[7]);
    return pg.ok ? pg.n : 0;
}

/* ---------------------------------------------------------------- HTTP Date */

/* n decimal digits at s, or -1 */
static int digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* index of the 3-letter name at s in names (exact case, as the RFC writes them), or -1 */
static int name3(const char *s, const char *names)
{
    for (int i = 0; names[3 * i]; i++) {
        if (memcmp(s, names + 3 * i, 3) == 0) {
            return i;
        }
    }
    return -1;
}

bool net_http_date(const char *s, size_t len, uint32_t *utc)
{
    while (len > 0 && (*s == ' ' || *s == '\t')) {
        s++, len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        len--;
    }
    /* "Sun, 06 Nov 1994 08:49:37 GMT": 29 bytes, every field at a fixed place */
    if (len != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' ||
        s[22] != ':' || s[25] != ' ' || memcmp(s + 26, "GMT", 3) != 0 || name3(s, "SunMonTueWedThuFriSat") < 0) {
        return false;
    }
    const int day = digits(s + 5, 2), month = name3(s + 8, "JanFebMarAprMayJunJulAugSepOctNovDec") + 1;
    const int year = digits(s + 12, 4), hour = digits(s + 17, 2), min = digits(s + 20, 2), sec = digits(s + 23, 2);
    if (year < 2000 || year > 2105 || month < 1 || day < 1 || day > board_days_in_month(year, (uint8_t)month) ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return false;
    }
    const board_datetime_t t = {.year = year, .month = (uint8_t)month, .day = (uint8_t)day, .hour = (uint8_t)hour,
                                .min = (uint8_t)min, .sec = (uint8_t)(sec == 60 ? 59 : sec)}; /* a leap second */
    *utc = (uint32_t)board_datetime_to_s(&t);
    return true;
}

/* ---------------------------------------------------------------- password */

void net_password(char *out, size_t len, uint32_t (*random32)(void))
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789"; /* 32: no l, o, 0, 1 */
    len = len < 8 ? 8 : len > 16 ? 16 : len;
    for (size_t i = 0; i < len; i++) {
        out[i] = alphabet[random32() & 31u]; /* 32 symbols: an unbiased 5 bits each */
    }
    out[len] = '\0';
}
