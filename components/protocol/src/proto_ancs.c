#include "epager/proto_ancs.h"

#include <string.h>

#include "epager/proto_tlv.h"

proto_status_t proto_ancs_parse_ns(const uint8_t *p, size_t len, proto_ancs_ns_t *out)
{
    if (len < 8) {
        return PROTO_MALFORMED;
    }
    out->event_id = p[0];
    out->event_flags = p[1];
    out->category_id = p[2];
    out->category_count = p[3];
    out->uid = proto_rd32(p + 4);
    return PROTO_OK;
}

static bool needs_max_len(uint8_t id)
{
    return id == PROTO_ANCS_ATTR_TITLE || id == PROTO_ANCS_ATTR_SUBTITLE || id == PROTO_ANCS_ATTR_MESSAGE;
}

size_t proto_ancs_build_get_notif_attrs(uint32_t uid, const proto_ancs_attr_req_t *attrs, size_t n, uint8_t *out,
                                        size_t cap)
{
    size_t need = 5;
    for (size_t i = 0; i < n; i++) {
        need += needs_max_len(attrs[i].id) ? 3 : 1;
    }
    if (need > cap) {
        return 0;
    }
    size_t k = 0;
    out[k++] = PROTO_ANCS_CMD_GET_NOTIF_ATTRS;
    proto_wr32(out + k, uid);
    k += 4;
    for (size_t i = 0; i < n; i++) {
        out[k++] = attrs[i].id;
        if (needs_max_len(attrs[i].id)) {
            proto_wr16(out + k, attrs[i].max_len);
            k += 2;
        }
    }
    return k;
}

size_t proto_ancs_build_get_app_attrs(const uint8_t *app_id, size_t app_id_len, const uint8_t *attr_ids, size_t n,
                                      uint8_t *out, size_t cap)
{
    size_t need = 1 + app_id_len + 1 + n;
    if (need > cap || memchr(app_id, 0, app_id_len)) {
        return 0;
    }
    out[0] = PROTO_ANCS_CMD_GET_APP_ATTRS;
    memcpy(out + 1, app_id, app_id_len);
    out[1 + app_id_len] = 0;
    memcpy(out + 2 + app_id_len, attr_ids, n);
    return need;
}

size_t proto_ancs_build_perform_action(uint32_t uid, uint8_t action_id, uint8_t *out, size_t cap)
{
    if (cap < 6) {
        return 0;
    }
    out[0] = PROTO_ANCS_CMD_PERFORM_ACTION;
    proto_wr32(out + 1, uid);
    out[5] = action_id;
    return 6;
}

static void ds_init(proto_ancs_ds_t *ds, const uint8_t *attr_ids, size_t n, uint8_t *buf, size_t cap)
{
    if (n > PROTO_ANCS_MAX_ATTRS) {
        n = PROTO_ANCS_MAX_ATTRS;
    }
    memcpy(ds->attr_ids, attr_ids, n);
    ds->n_attrs = (uint8_t)n;
    ds->buf = buf;
    ds->cap = cap;
}

void proto_ancs_ds_init_notif(proto_ancs_ds_t *ds, uint32_t uid, const uint8_t *attr_ids, size_t n, uint8_t *buf,
                              size_t cap)
{
    memset(ds, 0, sizeof(*ds));
    ds->command = PROTO_ANCS_CMD_GET_NOTIF_ATTRS;
    ds->uid = uid;
    ds_init(ds, attr_ids, n, buf, cap);
}

void proto_ancs_ds_init_app(proto_ancs_ds_t *ds, const uint8_t *app_id, size_t app_id_len, const uint8_t *attr_ids,
                            size_t n, uint8_t *buf, size_t cap)
{
    memset(ds, 0, sizeof(*ds));
    ds->command = PROTO_ANCS_CMD_GET_APP_ATTRS;
    ds->app_id = app_id;
    ds->app_id_len = app_id_len;
    ds_init(ds, attr_ids, n, buf, cap);
}

/* Parse what has arrived; *complete when every requested tuple is present. Responses carry
 * no fragment markers, so completeness is decided from the tuple structure. */
static proto_status_t ds_parse(proto_ancs_ds_t *ds, bool *complete)
{
    const uint8_t *b = ds->buf;
    size_t len = ds->len, i;
    *complete = false;
    if (len == 0) {
        return PROTO_OK;
    }
    if (b[0] != ds->command) {
        return PROTO_MALFORMED;
    }
    if (ds->command == PROTO_ANCS_CMD_GET_NOTIF_ATTRS) {
        if (len < 5) {
            return PROTO_OK;
        }
        if (proto_rd32(b + 1) != ds->uid) {
            return PROTO_MALFORMED;
        }
        i = 5;
    } else {
        const uint8_t *nul = memchr(b + 1, 0, len - 1);
        if (!nul) {
            return PROTO_OK;
        }
        size_t idlen = (size_t)(nul - (b + 1));
        if (idlen != ds->app_id_len || memcmp(b + 1, ds->app_id, idlen) != 0) {
            return PROTO_MALFORMED;
        }
        i = idlen + 2;
    }
    for (unsigned a = 0; a < ds->n_attrs; a++) {
        if (len - i < 3) {
            return PROTO_OK;
        }
        if (b[i] != ds->attr_ids[a]) {
            return PROTO_MALFORMED;
        }
        uint16_t alen = proto_rd16(b + i + 1);
        if (len - i - 3 < alen) {
            return PROTO_OK;
        }
        ds->attr_off[a] = (uint16_t)(i + 3);
        ds->attr_len[a] = alen;
        i += 3u + alen;
    }
    *complete = true;
    return PROTO_OK;
}

proto_status_t proto_ancs_ds_feed(proto_ancs_ds_t *ds, const uint8_t *frag, size_t len, bool *complete)
{
    *complete = false;
    if (ds->done) {
        return PROTO_OK;
    }
    if (len > ds->cap - ds->len || ds->cap > UINT16_MAX) {
        return PROTO_TOO_LARGE;
    }
    memcpy(ds->buf + ds->len, frag, len);
    ds->len += len;
    proto_status_t st = ds_parse(ds, complete);
    ds->done = *complete;
    return st;
}

bool proto_ancs_ds_attr(const proto_ancs_ds_t *ds, uint8_t attr_id, const uint8_t **val, uint16_t *len)
{
    if (!ds->done) {
        return false;
    }
    for (unsigned a = 0; a < ds->n_attrs; a++) {
        if (ds->attr_ids[a] == attr_id) {
            *val = ds->buf + ds->attr_off[a];
            *len = ds->attr_len[a];
            return true;
        }
    }
    return false;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static int num(const uint8_t *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

proto_status_t proto_ancs_parse_date(const uint8_t *s, size_t len, int64_t *local_epoch_s)
{
    if (len != 15 || s[8] != 'T') {
        return PROTO_MALFORMED;
    }
    for (size_t i = 0; i < 15; i++) {
        if (i != 8 && (s[i] < '0' || s[i] > '9')) {
            return PROTO_MALFORMED;
        }
    }
    int y = num(s, 4), mo = num(s + 4, 2), d = num(s + 6, 2);
    int h = num(s + 9, 2), mi = num(s + 11, 2), se = num(s + 13, 2);
    bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || h > 23 || mi > 59 || se > 59) {
        return PROTO_OUT_OF_RANGE;
    }
    int dim = mdays[mo - 1] + ((mo == 2 && leap) ? 1 : 0);
    if (d < 1 || d > dim) {
        return PROTO_OUT_OF_RANGE;
    }
    *local_epoch_s = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return PROTO_OK;
}
