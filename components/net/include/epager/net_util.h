/*
 * Portable helpers of the Wi-Fi task (PLAN D61, D63), host-tested: the setup hotspot's captive DNS answer,
 * form fields, HTML escaping and password, and the HTTP Date header. No ESP-IDF headers, no allocation;
 * every input is untrusted (from any phone that joined the hotspot, or any server's reply).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_DNS_MAX 512u /* a classic DNS message over UDP */

/* The answer to a DNS query: every A query (class IN) is answered with `ip` (TTL 60 s), any other
 * question with no records (NOERROR), so phones see a captive portal. Returns the answer's length,
 * or 0 to drop the packet (not a standard query with exactly one question, compression in the
 * question, a malformed or truncated name, or cap too small). */
size_t net_dns_answer(const uint8_t *q, size_t n, const uint8_t ip[4], uint8_t *out, size_t cap);

/* Field `name` of an application/x-www-form-urlencoded body ("a=1&b=x+y%21"), decoded ('+' is a space,
 * %XX a byte) into out[cap] (NUL-terminated). False if absent, badly escaped, not valid UTF-8, holding
 * a NUL or control character, or longer than cap - 1 bytes. */
bool net_form_get(const char *body, size_t len, const char *name, char *out, size_t cap);

/* `in` escaped for HTML text and attribute values (& < > " ') into out[cap], NUL-terminated and cut at
 * a whole character or entity. Returns the length written. */
size_t net_html_escape(const char *in, char *out, size_t cap);

#define NET_PAGE_MAX 8192u /* the setup page with NET_APS_MAX networks fits, every byte of every name escaped */
#define NET_APS_MAX 16u

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_ap_t;

typedef struct {
    bool vietnamese;           /* ui.language */
    const char *device;        /* the hotspot's name, the page title */
    const net_ap_t *aps;       /* networks from a scan, strongest first; at most NET_APS_MAX are listed */
    size_t n_aps;
    const char *ssid;          /* prefilled network (the stored one), may be "" */
    const char *city;          /* weather.city, may be "" */
    const char *note;          /* a line above the form, may be NULL */
} net_page_t;

/* The setup page (GET /): network (with the scanned networks as a list to tap), password (with a "show"
 * checkbox) and city, posted to /save as a form. Every value is
 * escaped. Returns the length written to out[cap] (NUL-terminated), 0 if it does not fit. */
size_t net_setup_page(char *out, size_t cap, const net_page_t *p);
/* The answer to POST /save: the board is joining `ssid`, its screen shows the outcome. */
size_t net_saved_page(char *out, size_t cap, bool vietnamese, const char *ssid);

/* The value of an HTTP Date header in the IMF-fixdate form every server sends (RFC 9110 5.6.7),
 * "Sun, 06 Nov 1994 08:49:37 GMT", to UTC seconds. Surrounding spaces are skipped. False for anything else
 * (the obsolete RFC 850 and asctime forms too), for a date that does not exist, or one outside 2000..2105
 * (UTC seconds fit in 32 bits). The time when SNTP does not answer (PLAN D63). */
bool net_http_date(const char *s, size_t len, uint32_t *utc);

/* A hotspot password of `len` characters (8..16) from an alphabet without look-alikes (no 0/O, 1/l/I),
 * drawn with `random32` (the hardware RNG on the board). out has len + 1 bytes. */
void net_password(char *out, size_t len, uint32_t (*random32)(void));

#ifdef __cplusplus
}
#endif
