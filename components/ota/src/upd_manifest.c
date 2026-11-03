/* The board's reading of manifest.json (contracts/releases.md). */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "epager/upd.h"
#include "epager/wx.h"

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_lower_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }

/* MAJOR.MINOR.PATCH, no leading zeros, at most 9 digits each; with_pre also allows -pre.release (no build metadata). */
static bool core_version_ok(const char *s, size_t n, bool with_pre)
{
    size_t pos = 0;
    for (int k = 0; k < 3; k++) {
        size_t start = pos;
        while (pos < n && is_digit(s[pos])) {
            pos++;
        }
        size_t digits = pos - start;
        if (digits == 0 || digits > 9 || (digits > 1 && s[start] == '0')) {
            return false;
        }
        if (k < 2) {
            if (pos >= n || s[pos++] != '.') {
                return false;
            }
        }
    }
    if (pos == n) {
        return true;
    }
    if (!with_pre || s[pos] != '-' || pos + 1 >= n) {
        return false;
    }
    for (pos++; pos < n; pos++) { /* identifiers of [0-9A-Za-z-], dot separated, none empty */
        char c = s[pos];
        bool dot = c == '.';
        bool id = is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
        if (!id && !(dot && pos + 1 < n && s[pos - 1] != '.' && s[pos + 1] != '.')) {
            return false;
        }
    }
    return true;
}

static bool get_string(const char *doc, size_t len, const char *path, char *out, size_t cap, size_t *n)
{
    wx_span_t v;
    /* wx_json_string cuts a long string: a value that does not fit is invalid here (no escapes in a valid manifest) */
    return wx_json_get(doc, len, path, &v) && v.n <= cap + 1 && wx_json_string(v, out, cap, n);
}

/* A JSON integer 1..2^32-1 (not a string, no fraction or exponent). */
static bool get_size(const char *doc, size_t len, const char *path, uint32_t *out)
{
    wx_span_t v;
    if (!wx_json_get(doc, len, path, &v) || v.n == 0 || v.n > 10) {
        return false;
    }
    uint64_t x = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (!is_digit(v.p[i])) {
            return false;
        }
        x = x * 10u + (uint64_t)(v.p[i] - '0');
    }
    if (x == 0 || x > UINT32_MAX || (v.n > 1 && v.p[0] == '0')) {
        return false;
    }
    *out = (uint32_t)x;
    return true;
}

static uint8_t variant_by_name(const char *name)
{
    if (strcmp(name, "paper") == 0) {
        return PROTO_VARIANT_PAPER;
    }
    return strcmp(name, "lcd") == 0 ? PROTO_VARIANT_LCD : PROTO_VARIANT_UNKNOWN;
}

static bool board_id_ok(const char *s, size_t n)
{
    if (n == 0 || n > 48 || s[0] == '-' || s[0] == '_') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool hex_to_sha(const char *s, size_t n, uint8_t out[PROTO_SHA256_LEN])
{
    if (n != 2 * PROTO_SHA256_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_lower_hex(s[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < PROTO_SHA256_LEN; i++) {
        unsigned hi = (unsigned)(is_digit(s[2 * i]) ? s[2 * i] - '0' : s[2 * i] - 'a' + 10);
        unsigned lo = (unsigned)(is_digit(s[2 * i + 1]) ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10);
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

typedef struct {
    char board_id[49];
    char variant[33];
    char file[UPD_FILE_MAX + 1];
    uint32_t size;
    uint8_t sha256[PROTO_SHA256_LEN];
} image_t;

/* One entry of images[]: false if it breaks the schema, or its file name is not epager-<board_id>-<version>.bin. */
static bool read_image(const char *doc, size_t len, unsigned i, const char *version, image_t *img)
{
    char path[32], key[48], hex[2 * PROTO_SHA256_LEN + 1];
    size_t n, bn;
    snprintf(path, sizeof(path), "images[%u].", i);
    size_t pl = strlen(path);
    memcpy(key, path, pl);
#define KEY(name) (memcpy(key + pl, name, sizeof(name)), key)
    if (!get_string(doc, len, KEY("board_id"), img->board_id, sizeof(img->board_id), &bn) ||
        !board_id_ok(img->board_id, bn)) {
        return false;
    }
    if (!get_string(doc, len, KEY("variant"), img->variant, sizeof(img->variant), &n) || n == 0) {
        return false;
    }
    for (size_t k = 0; k < n; k++) { /* schema: [a-z0-9_]{1,32} */
        char c = img->variant[k];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '_')) {
            return false;
        }
    }
    if (!get_string(doc, len, KEY("file"), img->file, sizeof(img->file), &n)) {
        return false;
    }
    char want[UPD_FILE_MAX + 1];
    int wn = snprintf(want, sizeof(want), "epager-%s-%s.bin", img->board_id, version);
    if (wn < 0 || (size_t)wn >= sizeof(want) || n != (size_t)wn || strcmp(img->file, want) != 0) {
        return false;
    }
    if (!get_size(doc, len, KEY("size"), &img->size)) {
        return false;
    }
    return get_string(doc, len, KEY("sha256"), hex, sizeof(hex), &n) && hex_to_sha(hex, n, img->sha256);
#undef KEY
}

static uint32_t major_of(const char *v)
{
    uint32_t x = 0;
    for (; is_digit(*v); v++) {
        x = x * 10u + (uint32_t)(*v - '0');
    }
    return x;
}

/* notes_url (optional, https) and min_app_contract (optional, MAJOR.MINOR.PATCH) when present. */
static bool optional_fields_ok(const char *doc, size_t len)
{
    wx_span_t v;
    if (wx_json_get(doc, len, "notes_url", &v) && (v.n < 10 || v.n > 514 || memcmp(v.p, "\"https://", 9) != 0)) {
        return false;
    }
    return true;
}

upd_verdict_t upd_decide(const char *json, size_t len, const upd_board_t *board, upd_image_t *out)
{
    char version[UPD_VERSION_MAX + 1], contract[16];
    size_t n;
    wx_span_t v;
    memset(out, 0, sizeof(*out));
    if (!wx_json_get(json, len, "manifest_version", &v) || v.n != 1 || v.p[0] != '1' ||
        !get_string(json, len, "version", version, sizeof(version), &n) || n == 0 ||
        !core_version_ok(version, n, true) ||
        !get_string(json, len, "contract_version", contract, sizeof(contract), &n) || !core_version_ok(contract, n, false) ||
        !optional_fields_ok(json, len)) {
        return UPD_INVALID;
    }
    image_t mine;
    bool found = false;
    char ids[UPD_IMAGES_MAX][49];
    unsigned count = 0;
    for (unsigned i = 0; i < UPD_IMAGES_MAX; i++) {
        image_t img;
        char path[40];
        snprintf(path, sizeof(path), "images[%u]", i);
        if (!wx_json_get(json, len, path, &v)) {
            break;
        }
        if (!read_image(json, len, i, version, &img)) {
            return UPD_INVALID;
        }
        snprintf(path, sizeof(path), "images[%u].min_app_contract", i);
        if (wx_json_get(json, len, path, &v)) {
            char mc[16];
            if (v.n > sizeof(mc) + 1 || !wx_json_string(v, mc, sizeof(mc), &n) || !core_version_ok(mc, n, false)) {
                return UPD_INVALID;
            }
        }
        for (unsigned k = 0; k < count; k++) {
            if (strcmp(ids[k], img.board_id) == 0) {
                return UPD_INVALID; /* a board_id twice */
            }
        }
        memcpy(ids[count++], img.board_id, sizeof(ids[0]));
        if (strcmp(img.board_id, board->board_id) == 0) {
            mine = img;
            found = true;
        }
    }
    if (count == 0) {
        return UPD_INVALID;
    }
    if (!found) {
        return UPD_NO_IMAGE;
    }
    snprintf(out->version, sizeof(out->version), "%s", version);
    if (variant_by_name(mine.variant) != board->variant) {
        return UPD_VARIANT_MISMATCH;
    }
    out->size = mine.size;
    if (mine.size > board->slot_size) {
        return UPD_TOO_LARGE;
    }
    if (major_of(contract) != major_of(board->contract_version)) {
        return UPD_CONTRACT_MAJOR;
    }
    if (strchr(version, '-') != NULL && !board->allow_prerelease) {
        return UPD_PRERELEASE;
    }
    int cmp;
    if (xfer_version_compare(version, strlen(version), board->running_version, strlen(board->running_version), &cmp) &&
        cmp <= 0) {
        return UPD_UP_TO_DATE;
    }
    snprintf(out->file, sizeof(out->file), "%s", mine.file);
    memcpy(out->sha256, mine.sha256, sizeof(out->sha256));
    return UPD_AVAILABLE;
}

/* ---------------------------------------------------------------- URLs */

bool upd_url_allowed(const char *url)
{
    static const char https[] = "https://";
    static const char suffix[] = ".githubusercontent.com";
    if (url == NULL || strncmp(url, https, sizeof(https) - 1) != 0) {
        return false;
    }
    const char *auth = url + sizeof(https) - 1;
    size_t n = strcspn(auth, "/?#");
    if (n > 3 && memcmp(auth + n - 4, ":443", 4) == 0) {
        n -= 4;
    }
    for (size_t i = 0; i < n; i++) { /* no user name, no other port, no odd characters */
        char c = auth[i];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) {
            return false;
        }
    }
    if (n == 10 && strncasecmp(auth, "github.com", 10) == 0) {
        return true;
    }
    return n > sizeof(suffix) && strncasecmp(auth + n - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0;
}

bool upd_image_url(const char *manifest_url, const char *version, const char *file, char *out, size_t cap)
{
    static const char latest[] = "/releases/latest/download/manifest.json";
    static const char leaf[] = "manifest.json";
    const size_t ul = strlen(manifest_url);
    int n;
    if (ul > sizeof(latest) - 1 && strcmp(manifest_url + ul - (sizeof(latest) - 1), latest) == 0) {
        n = snprintf(out, cap, "%.*s/releases/download/v%s/%s", (int)(ul - (sizeof(latest) - 1)), manifest_url, version, file);
    } else if (ul > sizeof(leaf) && strcmp(manifest_url + ul - (sizeof(leaf) - 1), leaf) == 0 &&
               manifest_url[ul - sizeof(leaf)] == '/') {
        n = snprintf(out, cap, "%.*s%s", (int)(ul - (sizeof(leaf) - 1)), manifest_url, file);
    } else {
        return false;
    }
    return n > 0 && (size_t)n < cap;
}
