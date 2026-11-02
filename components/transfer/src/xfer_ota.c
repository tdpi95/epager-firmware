#include "epager/xfer_ota.h"

#include <string.h>

/* ---------------------------------------------------------------- semver */

typedef struct {
    uint32_t num[3];
    const char *pre; /* pre-release identifiers, without the '-' */
    size_t pre_len;
} semver_t;

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_ident_char(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

static bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) {
            return false;
        }
    }
    return true;
}

/* Dot-separated, non-empty identifiers of [0-9A-Za-z-]; numeric ones (if checked) <= 9 digits. */
static bool idents_ok(const char *s, size_t n, bool check_numeric)
{
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            size_t len = i - start;
            if (len == 0 || (check_numeric && len > 9 && all_digits(s + start, len))) {
                return false;
            }
            start = i + 1;
        } else if (!is_ident_char(s[i])) {
            return false;
        }
    }
    return true;
}

static bool semver_parse(const char *s, size_t n, semver_t *v)
{
    size_t pos = 0;
    for (int k = 0; k < 3; k++) {
        size_t start = pos;
        uint32_t x = 0;
        while (pos < n && is_digit(s[pos]) && pos - start < 9) {
            x = x * 10u + (uint32_t)(s[pos++] - '0');
        }
        if (pos == start || (pos < n && is_digit(s[pos]))) {
            return false;
        }
        v->num[k] = x;
        if (k < 2 && (pos >= n || s[pos++] != '.')) {
            return false;
        }
    }
    v->pre = NULL;
    v->pre_len = 0;
    if (pos < n && s[pos] == '-') {
        size_t start = ++pos;
        while (pos < n && s[pos] != '+') {
            pos++;
        }
        v->pre = s + start;
        v->pre_len = pos - start;
        if (!idents_ok(v->pre, v->pre_len, true)) {
            return false;
        }
    }
    if (pos < n && s[pos] == '+') {
        pos++;
        if (!idents_ok(s + pos, n - pos, false)) {
            return false;
        }
        pos = n;
    }
    return pos == n;
}

static size_t ident_len(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] != '.') {
        i++;
    }
    return i;
}

static uint32_t ident_value(const char *s, size_t n)
{
    uint32_t x = 0;
    for (size_t i = 0; i < n; i++) {
        x = x * 10u + (uint32_t)(s[i] - '0');
    }
    return x;
}

static int cmp_ident(const char *a, size_t an, const char *b, size_t bn)
{
    bool ad = all_digits(a, an), bd = all_digits(b, bn);
    if (ad && bd && ident_value(a, an) != ident_value(b, bn)) {
        return ident_value(a, an) < ident_value(b, bn) ? -1 : 1;
    }
    if (ad != bd) {
        return ad ? -1 : 1; /* numeric identifiers sort first */
    }
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c) {
        return c < 0 ? -1 : 1;
    }
    return (an > bn) - (an < bn);
}

bool xfer_version_compare(const char *a, size_t alen, const char *b, size_t blen, int *cmp)
{
    semver_t x, y;
    if (!semver_parse(a, alen, &x) || !semver_parse(b, blen, &y)) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        if (x.num[k] != y.num[k]) {
            *cmp = x.num[k] < y.num[k] ? -1 : 1;
            return true;
        }
    }
    if (!x.pre_len || !y.pre_len) {
        *cmp = (!x.pre_len) - (!y.pre_len); /* a release is newer than its pre-releases */
        return true;
    }
    const char *p = x.pre, *q = y.pre;
    size_t pn = x.pre_len, qn = y.pre_len;
    for (;;) {
        size_t il = ident_len(p, pn), jl = ident_len(q, qn);
        int c = cmp_ident(p, il, q, jl);
        if (c) {
            *cmp = c;
            return true;
        }
        bool more_p = il < pn, more_q = jl < qn;
        if (!more_p || !more_q) {
            *cmp = (int)more_p - (int)more_q;
            return true;
        }
        p += il + 1;
        pn -= il + 1;
        q += jl + 1;
        qn -= jl + 1;
    }
}

/* ---------------------------------------------------------------- state machine */

static void emit(xfer_ota_t *o, xfer_ota_event_kind_t kind)
{
    xfer_ota_event_t ev = {kind, o->last_pct, o->state};
    if (o->p.on_event) {
        o->p.on_event(o->p.ctx, &ev);
    }
}

static void set_state(xfer_ota_t *o, uint8_t state)
{
    if (o->state != state) {
        o->state = state;
        emit(o, XFER_OTA_EV_STATE);
    }
}

void xfer_ota_init(xfer_ota_t *o, const xfer_ota_params_t *p)
{
    memset(o, 0, sizeof(*o));
    o->p = *p;
    o->state = PROTO_OTA_STATE_NONE;
    o->last_pct = 0xFF;
}

proto_status_t xfer_ota_check_battery(xfer_ota_t *o)
{
    if (!o->p.read_battery) {
        return PROTO_OK; /* no battery reader */
    }
    uint8_t pct = 0, ext = PROTO_EXTERNAL_POWER_UNKNOWN;
    if (!o->p.read_battery(o->p.ctx, &pct, &ext)) {
        return PROTO_LOW_BATTERY;
    }
    return ext == PROTO_EXTERNAL_POWER_YES || pct >= o->p.low_battery_pct ? PROTO_OK : PROTO_LOW_BATTERY;
}

proto_status_t xfer_ota_check_begin(xfer_ota_t *o, const char *version, size_t len, bool has_version)
{
    const char *run = o->p.running_version;
    semver_t v;
    int cmp;
    if (!has_version || !semver_parse(version, len, &v) ||
        (!o->p.allow_downgrade && (!xfer_version_compare(version, len, run, strlen(run), &cmp) || cmp <= 0))) {
        return PROTO_VERSION_REJECTED;
    }
    return xfer_ota_check_battery(o);
}

void xfer_ota_progress(xfer_ota_t *o, uint32_t received, uint32_t total)
{
    uint8_t pct = total ? (uint8_t)((uint64_t)received * 100u / total) : 100u;
    if (pct != o->last_pct) {
        o->last_pct = pct;
        emit(o, XFER_OTA_EV_PROGRESS);
    }
}

void xfer_ota_installed(xfer_ota_t *o, const char *version, size_t len)
{
    uint8_t rec[3 + XFER_OTA_VERSION_MAX] = {'O', 1};
    len = len > XFER_OTA_VERSION_MAX ? XFER_OTA_VERSION_MAX : len;
    rec[2] = (uint8_t)len;
    memcpy(rec + 3, version, len);
    /* If this fails the update still happens; only the rolled_back/updated report is lost. */
    (void)o->p.kv->set(o->p.kv->ctx, XFER_OTA_KV_KEY, rec, 3 + len);
    o->last_pct = 0xFF;
    emit(o, XFER_OTA_EV_REBOOT);
}

void xfer_ota_boot(xfer_ota_t *o, bool image_pending_verify, uint32_t now_ms)
{
    uint8_t rec[3 + XFER_OTA_VERSION_MAX];
    size_t n = 0;
    bool has_rec = o->p.kv->get(o->p.kv->ctx, XFER_OTA_KV_KEY, rec, sizeof(rec), &n) == KV_OK && n >= 3 &&
                   rec[0] == 'O' && rec[1] == 1 && rec[2] == n - 3;
    o->passed = 0;
    o->rollback_requested = false;
    if (image_pending_verify) {
        o->verify_start_ms = now_ms;
        set_state(o, PROTO_OTA_STATE_PENDING_VERIFY);
        xfer_ota_check_passed(o, 0); /* nothing required: valid at once */
        return;
    }
    if (has_rec) {
        const char *run = o->p.running_version;
        bool updated = strlen(run) == rec[2] && memcmp(run, rec + 3, rec[2]) == 0;
        (void)o->p.kv->erase(o->p.kv->ctx, XFER_OTA_KV_KEY);
        set_state(o, updated ? PROTO_OTA_STATE_UPDATED : PROTO_OTA_STATE_ROLLED_BACK);
    }
}

void xfer_ota_check_passed(xfer_ota_t *o, uint32_t check_bits)
{
    if (o->state != PROTO_OTA_STATE_PENDING_VERIFY || o->rollback_requested) {
        return;
    }
    o->passed |= check_bits;
    if ((o->passed & o->p.required_checks) == o->p.required_checks) {
        emit(o, XFER_OTA_EV_MARK_VALID);
        (void)o->p.kv->erase(o->p.kv->ctx, XFER_OTA_KV_KEY);
        set_state(o, PROTO_OTA_STATE_UPDATED);
    }
}

void xfer_ota_check_failed(xfer_ota_t *o)
{
    if (o->state == PROTO_OTA_STATE_PENDING_VERIFY && !o->rollback_requested) {
        o->rollback_requested = true;
        emit(o, XFER_OTA_EV_ROLLBACK);
    }
}

void xfer_ota_tick(xfer_ota_t *o, uint32_t now_ms)
{
    if (o->state == PROTO_OTA_STATE_PENDING_VERIFY && now_ms - o->verify_start_ms >= o->p.verify_timeout_ms) {
        xfer_ota_check_failed(o);
    }
}

uint8_t xfer_ota_state(const xfer_ota_t *o) { return o->state; }
