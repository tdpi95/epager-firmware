/* Portable helpers of the Wi-Fi setup hotspot (components/net/src/net_util.c): the captive DNS answer,
 * form fields, HTML escaping, the setup pages and the hotspot password. Inputs come from any phone on the
 * hotspot, so the malformed cases matter as much as the good ones. */
#include <stdlib.h>
#include <string.h>

#include "epager/net_util.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t IP[4] = {192, 168, 4, 1};

/* A query for `name` ("connectivitycheck.gstatic.com") with type and class. */
static size_t query(uint8_t *q, const char *name, uint16_t type, uint16_t klass)
{
    const uint8_t head[12] = {0xAB, 0xCD, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0}; /* id, RD, one question */
    memcpy(q, head, sizeof(head));
    size_t n = sizeof(head);
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        const size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, l);
        n += l;
        p += l + (dot != NULL);
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(type >> 8);
    q[n++] = (uint8_t)type;
    q[n++] = (uint8_t)(klass >> 8);
    q[n++] = (uint8_t)klass;
    return n;
}

static void test_dns_answers_every_a_query(void)
{
    uint8_t q[NET_DNS_MAX], a[NET_DNS_MAX];
    size_t n = query(q, "connectivitycheck.gstatic.com", 1, 1);
    size_t m = net_dns_answer(q, n, IP, a, sizeof(a));
    TEST_ASSERT_EQUAL_UINT(n + 16, m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(q, a, 2); /* the id */
    TEST_ASSERT_EQUAL_HEX8(0x85, a[2]);    /* QR, AA, RD as asked */
    TEST_ASSERT_EQUAL_HEX8(0x00, a[3]);    /* NOERROR */
    TEST_ASSERT_EQUAL_HEX8(1, a[7]);       /* one answer */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(q + 12, a + 12, n - 12); /* the question */
    const uint8_t rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rr, a + n, 16);

    n = query(q, "captive.apple.com", 28, 1); /* AAAA: no records, still NOERROR */
    m = net_dns_answer(q, n, IP, a, sizeof(a));
    TEST_ASSERT_EQUAL_UINT(n, m);
    TEST_ASSERT_EQUAL_HEX8(0, a[7]);
    TEST_ASSERT_EQUAL_HEX8(0, a[3]);
    n = query(q, "a", 1, 1);
    q[2] = 0x00; /* RD clear stays clear */
    TEST_ASSERT_NOT_EQUAL(0, net_dns_answer(q, n, IP, a, sizeof(a)));
    TEST_ASSERT_EQUAL_HEX8(0x84, a[2]);
    /* additional records (EDNS) are dropped from the answer */
    n = query(q, "x.y", 1, 1);
    q[11] = 1;
    const uint8_t opt[11] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(q + n, opt, sizeof(opt));
    m = net_dns_answer(q, n + sizeof(opt), IP, a, sizeof(a));
    TEST_ASSERT_EQUAL_UINT(n + 16, m);
    TEST_ASSERT_EQUAL_HEX8(0, a[11]);
}

static void test_dns_drops_bad_packets(void)
{
    uint8_t q[NET_DNS_MAX + 8], a[NET_DNS_MAX];
    size_t n = query(q, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, 11, IP, a, sizeof(a)));       /* shorter than a header */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, n - 1, IP, a, sizeof(a)));    /* class cut */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, n, IP, a, n + 15));           /* answer does not fit */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, NET_DNS_MAX + 1, IP, a, sizeof(a)));
    for (size_t cut = 12; cut < n; cut++) {
        TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, cut, IP, a, sizeof(a)));
    }
    uint8_t b[NET_DNS_MAX];
    memcpy(b, q, n);
    b[2] |= 0x80; /* a response */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));
    memcpy(b, q, n);
    b[2] |= 0x28; /* opcode 5 (update) */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));
    memcpy(b, q, n);
    b[5] = 2; /* two questions */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));
    memcpy(b, q, n);
    b[7] = 1; /* an answer in a query */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));
    memcpy(b, q, n);
    b[12] = 0xC0; /* a compression pointer in the question */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));
    memcpy(b, q, n);
    b[12] = 64; /* label too long */
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(b, n, IP, a, sizeof(a)));

    /* a name over 255 bytes: five 63-byte labels */
    char longname[5 * 64];
    for (int i = 0; i < 5; i++) {
        memset(longname + i * 64, 'a', 63);
        longname[i * 64 + 63] = i < 4 ? '.' : '\0';
    }
    n = query(q, longname, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, net_dns_answer(q, n, IP, a, sizeof(a)));
}

static void test_dns_fuzz(void)
{
    uint8_t q[NET_DNS_MAX], a[NET_DNS_MAX];
    uint32_t seed = 99;
    for (int it = 0; it < 50000; it++) {
        size_t n = query(q, "connectivitycheck.gstatic.com", 1, 1);
        for (int k = 0; k < 3; k++) {
            seed = seed * 1103515245u + 12345u;
            q[(seed >> 8) % n] = (uint8_t)(seed >> 20);
        }
        seed = seed * 1103515245u + 12345u;
        n = (seed >> 9) % (n + 1);
        size_t m = net_dns_answer(q, n, IP, a, sizeof(a));
        /* an answer is the header and question (trailing records dropped), plus at most one record */
        TEST_ASSERT_TRUE(m == 0 || (m >= 12 + 5 && m <= n + 16));
    }
}

static void test_form_fields(void)
{
    char out[65];
    const char body[] = "ssid=Qu%C3%A1n+Cafe+5G&pw=p%40ss+w0rd%26%3D&city=&x";
    TEST_ASSERT_TRUE(net_form_get(body, strlen(body), "ssid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Quán Cafe 5G", out);
    TEST_ASSERT_TRUE(net_form_get(body, strlen(body), "pw", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("p@ss w0rd&=", out);
    TEST_ASSERT_TRUE(net_form_get(body, strlen(body), "city", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(net_form_get(body, strlen(body), "x", out, sizeof(out)));   /* no '=' */
    TEST_ASSERT_FALSE(net_form_get(body, strlen(body), "ss", out, sizeof(out)));  /* a prefix is not a name */
    TEST_ASSERT_FALSE(net_form_get(body, strlen(body), "zz", out, sizeof(out)));
    TEST_ASSERT_TRUE(net_form_get(body, 7, "ssid", out, sizeof(out)));           /* the body ends there */
    TEST_ASSERT_EQUAL_STRING("Qu", out);
    TEST_ASSERT_FALSE(net_form_get(body, 9, "ssid", out, sizeof(out)));          /* inside "%C3" */

    const char *bad[] = {"a=%", "a=%4", "a=%zz", "a=%00", "a=%0A", "a=%7F", "a=%C3", "a=%C3%28", "a=\t"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(net_form_get(bad[i], strlen(bad[i]), "a", out, sizeof(out)), bad[i]);
    }
    char small[4];
    TEST_ASSERT_TRUE(net_form_get("a=abc", 5, "a", small, sizeof(small)));
    TEST_ASSERT_FALSE(net_form_get("a=abcd", 6, "a", small, sizeof(small)));
}

static void test_html_escape(void)
{
    char out[64];
    const char *want = "&lt;a href=&quot;x&quot;&gt;&#39;Tom &amp; Jerry&#39;&lt;/a&gt;";
    TEST_ASSERT_EQUAL_UINT(strlen(want), net_html_escape("<a href=\"x\">'Tom & Jerry'</a>", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_UINT(1, net_html_escape("a&b", out, 6)); /* "a&amp;" needs 7 bytes: cut before the entity */
    TEST_ASSERT_EQUAL_STRING("a", out);
    net_html_escape("Hà", out, 3); /* à is 2 bytes: only "H" fits with the NUL */
    TEST_ASSERT_EQUAL_STRING("H", out);
    net_html_escape("a\xff" "b\xc3", out, sizeof(out)); /* invalid UTF-8 becomes U+FFFD */
    TEST_ASSERT_EQUAL_STRING("a\xef\xbf\xbd" "b\xef\xbf\xbd", out);
    TEST_ASSERT_EQUAL_UINT(0, net_html_escape("x", out, 0));
}

static void test_pages(void)
{
    static char page[NET_PAGE_MAX];
    net_ap_t aps[NET_APS_MAX + 2];
    memset(aps, 0, sizeof(aps));
    snprintf(aps[0].ssid, sizeof(aps[0].ssid), "Quán Cafe 5G");
    aps[0].rssi = -50; /* secured, strong */
    snprintf(aps[1].ssid, sizeof(aps[1].ssid), "\"><script>x</script>");
    aps[1].rssi = -90;
    aps[1].open = true;
    for (size_t i = 2; i < NET_APS_MAX + 2; i++) {
        memset(aps[i].ssid, '"', 32); /* the worst case: every byte escaped to its longest form (&quot;) */
        aps[i].rssi = -70;
    }
    net_page_t p = {.device = "ePager-1A2B", .aps = aps, .n_aps = NET_APS_MAX + 2, .ssid = "Quán Cafe 5G",
                    .city = "Hà Nội", .note = "Could not join: wrong password?"};
    size_t n = net_setup_page(page, sizeof(page), &p);
    TEST_ASSERT_TRUE(n > 0 && n < sizeof(page));
    TEST_ASSERT_EQUAL_UINT(strlen(page), n);
    TEST_ASSERT_NOT_NULL(strstr(page, "<title>ePager-1A2B</title>"));
    TEST_ASSERT_NOT_NULL(strstr(page, "value=\"Quán Cafe 5G\""));
    /* scanned networks: a list to tap, names escaped, signal bars, a lock unless open */
    TEST_ASSERT_NOT_NULL(strstr(page, "class=n><b>Quán Cafe 5G</b><span>▂▄▆█ 🔒</span>"));
    TEST_ASSERT_NOT_NULL(strstr(page, "class=n><b>&quot;&gt;&lt;script&gt;x&lt;/script&gt;</b><span>▂</span>"));
    TEST_ASSERT_NOT_NULL(strstr(page, "</b><span>▂▄ 🔒</span>"));
    TEST_ASSERT_NULL(strstr(page, "<script"));
    TEST_ASSERT_NOT_NULL(strstr(page, "Networks nearby: tap one to choose it"));
    /* the password field and its "show" checkbox, which has no name and is never posted */
    TEST_ASSERT_NOT_NULL(strstr(page, "<input name=pw type=password maxlength=63>"));
    TEST_ASSERT_NOT_NULL(strstr(page, "<input type=checkbox onchange=\"this.form.pw.type=this.checked?'text':'password'\">Show password"));
    TEST_ASSERT_NOT_NULL(strstr(page, "name=city maxlength=64 value=\"Hà Nội\""));
    TEST_ASSERT_NOT_NULL(strstr(page, "Open-Meteo.com (CC BY 4.0)"));
    TEST_ASSERT_NOT_NULL(strstr(page, "<b>Could not join: wrong password?</b>"));
    size_t listed = 0;
    for (const char *s = page; (s = strstr(s, "class=n>")); s++) {
        listed++;
    }
    TEST_ASSERT_EQUAL_UINT(NET_APS_MAX, listed);
    TEST_ASSERT_EQUAL_UINT(0, net_setup_page(page, 200, &p)); /* does not fit */

    p.vietnamese = true;
    p.note = NULL;
    TEST_ASSERT_NOT_EQUAL(0, net_setup_page(page, sizeof(page), &p));
    TEST_ASSERT_NOT_NULL(strstr(page, "Mạng Wi-Fi"));
    TEST_ASSERT_NOT_NULL(strstr(page, "Hiện mật khẩu"));
    TEST_ASSERT_NULL(strstr(page, "<p><b>")); /* no note */
    p.n_aps = 0;
    TEST_ASSERT_NOT_EQUAL(0, net_setup_page(page, sizeof(page), &p));
    TEST_ASSERT_NULL(strstr(page, "class=n>")); /* nothing scanned: no list and no heading */
    TEST_ASSERT_NULL(strstr(page, "Mạng xung quanh"));

    n = net_saved_page(page, sizeof(page), false, "<Home>");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(page, "ePager is joining <b>&lt;Home&gt;</b>."));
}

static uint32_t rng_state;
static uint32_t rng(void) { return rng_state = rng_state * 1664525u + 1013904223u; }

static uint32_t date(const char *s)
{
    uint32_t utc = 12345;
    return net_http_date(s, strlen(s), &utc) ? utc : 0;
}

static void test_http_date(void)
{
    /* expected values from Python's calendar.timegm */
    TEST_ASSERT_EQUAL_UINT32(1791976596u, date("Wed, 14 Oct 2026 11:16:36 GMT"));
    TEST_ASSERT_EQUAL_UINT32(946684800u, date("Sat, 01 Jan 2000 00:00:00 GMT"));   /* first accepted */
    TEST_ASSERT_EQUAL_UINT32(4291747199u, date("Mon, 31 Dec 2105 23:59:59 GMT"));  /* last accepted */
    TEST_ASSERT_EQUAL_UINT32(1835438400u, date("Tue, 29 Feb 2028 12:00:00 GMT"));  /* leap day */
    TEST_ASSERT_EQUAL_UINT32(951782400u, date("Tue, 29 Feb 2000 00:00:00 GMT"));   /* 400-year leap */
    TEST_ASSERT_EQUAL_UINT32(1798761599u, date("Thu, 31 Dec 2026 23:59:60 GMT"));  /* leap second: :59 */
    TEST_ASSERT_EQUAL_UINT32(1791976596u, date("  Wed, 14 Oct 2026 11:16:36 GMT \r\n")); /* spaces, CRLF */

    const char *bad[] = {
        "Sun, 06 Nov 1994 08:49:37 GMT",   /* before 2000 */
        "Sat, 01 Jan 2106 00:00:00 GMT",   /* past 32-bit seconds */
        "Sunday, 06-Nov-26 08:49:37 GMT",  /* RFC 850 (obsolete) */
        "Sun Nov  6 08:49:37 2026",        /* asctime (obsolete) */
        "Thu, 29 Feb 2026 00:00:00 GMT",   /* not a leap year */
        "Mon, 29 Feb 2100 00:00:00 GMT",   /* century, not a leap year */
        "Wed, 31 Apr 2026 00:00:00 GMT", "Wed, 00 Oct 2026 00:00:00 GMT", "Wed, 32 Oct 2026 00:00:00 GMT",
        "Wed, 14 Oct 2026 24:00:00 GMT", "Wed, 14 Oct 2026 11:60:00 GMT", "Wed, 14 Oct 2026 11:16:61 GMT",
        "Wed, 14 Oct 2026 11:16:36 UTC", "Wed, 14 Oct 2026 11:16:36 gmt", "wed, 14 Oct 2026 11:16:36 GMT",
        "Wed, 14 Okt 2026 11:16:36 GMT", "Xyz, 14 Oct 2026 11:16:36 GMT", "Wed, 1a Oct 2026 11:16:36 GMT",
        "Wed, 14 Oct 2026 11-16-36 GMT", "Wed,14 Oct 2026 11:16:36 GMT", "Wed, 14 Oct 2026 11:16:36 GMTX",
        "Wed, 14 Oct 26 11:16:36 GMT", "Wed, -4 Oct 2026 11:16:36 GMT", "", "   ",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, date(bad[i]), bad[i]);
    }
    /* the length is the caller's: a NUL or a cut inside the value is not a date */
    TEST_ASSERT_FALSE(net_http_date("Wed, 14 Oct 2026 11:16:36 GMT", 28, &(uint32_t){0}));
    TEST_ASSERT_FALSE(net_http_date("Wed, 14 Oct\0 2026 11:16:36 GMT", 30, &(uint32_t){0}));
    uint32_t keep = 7;
    TEST_ASSERT_FALSE(net_http_date("garbage", 7, &keep));
    TEST_ASSERT_EQUAL_UINT32(7, keep); /* untouched on failure */

    /* mutations of a valid value: never a crash or a read past the length (ASan), only valid dates */
    char m[40];
    uint32_t seed = 1;
    for (int i = 0; i < 20000; i++) {
        memcpy(m, "Wed, 14 Oct 2026 11:16:36 GMT", 29);
        seed = seed * 1664525u + 1013904223u;
        m[(seed >> 8) % 29] = (char)(seed >> 16);
        const size_t n = 20 + (seed >> 24) % 10; /* also shorter than the form */
        uint32_t utc;
        if (net_http_date(m, n, &utc)) {
            TEST_ASSERT_TRUE(utc >= 946684800u);
        }
    }
}

static void test_password(void)
{
    char pw[17];
    rng_state = 1;
    net_password(pw, 10, rng);
    TEST_ASSERT_EQUAL_UINT(10, strlen(pw));
    TEST_ASSERT_EQUAL_UINT(10, strspn(pw, "abcdefghijkmnpqrstuvwxyz23456789"));
    net_password(pw, 3, rng);
    TEST_ASSERT_EQUAL_UINT(8, strlen(pw)); /* WPA2 minimum */
    net_password(pw, 40, rng);
    TEST_ASSERT_EQUAL_UINT(16, strlen(pw));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dns_answers_every_a_query);
    RUN_TEST(test_dns_drops_bad_packets);
    RUN_TEST(test_dns_fuzz);
    RUN_TEST(test_form_fields);
    RUN_TEST(test_html_escape);
    RUN_TEST(test_pages);
    RUN_TEST(test_http_date);
    RUN_TEST(test_password);
    return UNITY_END();
}
