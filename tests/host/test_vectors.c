/* Runs the C protocol library against every case in contracts/test-vectors/. */
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "unity.h"
#include "xboard.h"
#include "xcore.h"

#define BUF 8192

void setUp(void) { g_case_failures = 0; }
void tearDown(void) {}

#define FINISH_CASES() TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_case_failures, "failing vector cases (see stderr)")

static const cJSON *cases_of(const cJSON *doc, const char *key) { return cJSON_GetObjectItemCaseSensitive(doc, key); }

/* ---------------------------------------------------------------- messages.json */

static void test_messages(void)
{
    cJSON *doc = load_vectors("messages.json");
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        const char *name = jstr(c, "name");
        uint8_t buf[BUF], enc[BUF];
        size_t len = hex_decode(jstr(c, "hex"), buf, sizeof(buf));
        proto_status_t want = status_by_name(jstr(c, "expect"));
        proto_msg_t m;
        proto_status_t got = proto_msg_parse(buf, len, &m);
        n++;
        if (got != want) {
            CASE_FAIL(name, "status %s, want %s", proto_status_name(got), proto_status_name(want));
            continue;
        }
        const cJSON *logical = cJSON_GetObjectItemCaseSensitive(c, "logical");
        if (got != PROTO_OK || !logical) {
            continue;
        }
        cJSON *dec = msg_to_json(&m);
        if (!cJSON_Compare(dec, logical, true)) {
            char *s = cJSON_PrintUnformatted(dec);
            CASE_FAIL(name, "decoded %s", s);
            free(s);
        }
        cJSON_Delete(dec);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "decode_only"))) {
            continue;
        }
        size_t elen = json_to_msg(logical, enc, sizeof(enc));
        if (elen != len || memcmp(enc, buf, len) != 0) {
            char hex[2 * BUF + 1];
            hex_encode(enc, elen, hex);
            CASE_FAIL(name, "encoded %s", hex);
        }
    }
    cJSON_Delete(doc);
    printf("messages: %d cases\n", n);
    FINISH_CASES();
}

static void test_capabilities_fit_att_value(void)
{
    cJSON *doc = load_vectors("messages.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(c, "logical");
        const char *type = l ? jstr(l, "type") : NULL;
        if (type && strcmp(type, "CAPABILITIES") == 0) {
            TEST_ASSERT_LESS_OR_EQUAL(PROTO_LIMIT_CAPABILITIES_MAX, strlen(jstr(c, "hex")) / 2);
        }
    }
    cJSON_Delete(doc);
}

/* ---------------------------------------------------------------- segments.json */

static void test_segments_split(void)
{
    cJSON *doc = load_vectors("segments.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "split")) {
        const char *name = jstr(c, "name");
        uint8_t msg[BUF], seg[BUF], want[BUF];
        size_t len = hex_decode(jstr(c, "message"), msg, sizeof(msg));
        size_t maxseg = (size_t)jnum(c, "max_segment", 0);
        proto_seg_tx_t tx;
        proto_seg_tx_init(&tx, msg, len, maxseg);
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(c, "segments");
        int i = 0;
        size_t n;
        while ((n = proto_seg_tx_next(&tx, seg, sizeof(seg))) > 0) {
            const cJSON *e = cJSON_GetArrayItem(exp, i);
            size_t wl = e ? hex_decode(e->valuestring, want, sizeof(want)) : 0;
            if (!e || wl != n || memcmp(want, seg, n) != 0) {
                CASE_FAIL(name, "segment %d differs", i);
                break;
            }
            i++;
        }
        if (i != cJSON_GetArraySize(exp)) {
            CASE_FAIL(name, "%d segments, want %d", i, cJSON_GetArraySize(exp));
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

static void test_segments_reassemble(void)
{
    cJSON *doc = load_vectors("segments.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "reassemble")) {
        const char *name = jstr(c, "name");
        size_t maxm = (size_t)jnum(c, "max_message", 0);
        uint8_t *rxbuf = malloc(maxm ? maxm : 1); /* exact size so ASan catches overruns */
        proto_seg_rx_t rx;
        proto_seg_rx_init(&rx, rxbuf, maxm);
        cJSON *got = cJSON_CreateArray();
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(c, "segments")) {
            uint8_t seg[BUF];
            size_t sl = hex_decode(s->valuestring, seg, sizeof(seg));
            uint8_t *exact = malloc(sl ? sl : 1);
            memcpy(exact, seg, sl);
            proto_seg_event_t ev[2];
            size_t ne = proto_seg_rx_feed(&rx, exact, sl, ev);
            for (size_t k = 0; k < ne; k++) {
                cJSON *o = cJSON_CreateObject();
                if (ev[k].status == PROTO_OK) {
                    char hex[2 * BUF + 1];
                    hex_encode(ev[k].msg, ev[k].len, hex);
                    cJSON_AddStringToObject(o, "message", hex);
                } else {
                    cJSON_AddStringToObject(o, "error", proto_status_name(ev[k].status));
                }
                cJSON_AddItemToArray(got, o);
            }
            free(exact);
        }
        if (!cJSON_Compare(got, cJSON_GetObjectItemCaseSensitive(c, "events"), true)) {
            char *p = cJSON_PrintUnformatted(got);
            CASE_FAIL(name, "events %s", p);
            free(p);
        }
        cJSON_Delete(got);
        free(rxbuf);
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- config_values.json */

static uint8_t variant_by_name(const char *v)
{
    if (strcmp(v, "paper") == 0) return PROTO_VARIANT_PAPER;
    if (strcmp(v, "lcd") == 0) return PROTO_VARIANT_LCD;
    return PROTO_VARIANT_UNKNOWN;
}

static void test_config_values(void)
{
    cJSON *doc = load_vectors("config_values.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        uint8_t val[256];
        size_t len = hex_decode(jstr(c, "value_hex"), val, sizeof(val));
        proto_status_t got = proto_cfg_validate((uint16_t)jnum(c, "key", 0), val, len, variant_by_name(jstr(c, "variant")));
        proto_status_t want = status_by_name(jstr(c, "expect"));
        if (got != want) {
            CASE_FAIL(jstr(c, "name"), "status %s, want %s", proto_status_name(got), proto_status_name(want));
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- text.json */

static void test_text(void)
{
    cJSON *doc = load_vectors("text.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        uint8_t in[BUF], want[BUF];
        size_t inl = hex_decode(jstr(c, "input_hex"), in, sizeof(in));
        size_t wl = hex_decode(jstr(c, "output_hex"), want, sizeof(want));
        double b = jnum(c, "budget", -1);
        size_t budget = b < 0 ? PROTO_TEXT_NO_BUDGET : (size_t)b;
        size_t cap = 4 * inl + 8;
        uint8_t *out = malloc(cap);
        size_t n = proto_text_normalize(in, inl, out, cap, budget, NULL, NULL);
        if (n != wl || memcmp(out, want, n) != 0) {
            char hex[2 * BUF + 1];
            hex_encode(out, n, hex);
            CASE_FAIL(jstr(c, "name"), "output %s", hex);
        }
        free(out);
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- ancs.json */

static void test_ancs_notification_source(void)
{
    cJSON *doc = load_vectors("ancs.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "notification_source")) {
        const char *name = jstr(c, "name");
        uint8_t p[64];
        size_t len = hex_decode(jstr(c, "hex"), p, sizeof(p));
        proto_ancs_ns_t ns;
        proto_status_t got = proto_ancs_parse_ns(p, len, &ns);
        if (got != status_by_name(jstr(c, "expect"))) {
            CASE_FAIL(name, "status %s", proto_status_name(got));
            continue;
        }
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(c, "parsed");
        if (e && (ns.event_id != jnum(e, "event_id", -1) || ns.event_flags != jnum(e, "event_flags", -1) ||
                  ns.category_id != jnum(e, "category_id", -1) || ns.category_count != jnum(e, "category_count", -1) ||
                  ns.uid != (uint32_t)jnum(e, "uid", -1))) {
            CASE_FAIL(name, "parsed fields differ");
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

static void test_ancs_requests(void)
{
    cJSON *doc = load_vectors("ancs.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "requests")) {
        uint8_t out[256], want[256];
        size_t wl = hex_decode(jstr(c, "hex"), want, sizeof(want)), n = 0;
        int cmd = (int)jnum(c, "command", -1);
        const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(c, "attributes");
        if (cmd == PROTO_ANCS_CMD_GET_NOTIF_ATTRS) {
            proto_ancs_attr_req_t req[PROTO_ANCS_MAX_ATTRS];
            size_t k = 0;
            const cJSON *a;
            cJSON_ArrayForEach(a, attrs) {
                req[k].id = (uint8_t)jnum(a, "id", 0);
                req[k].max_len = (uint16_t)jnum(a, "max_len", 0);
                k++;
            }
            n = proto_ancs_build_get_notif_attrs((uint32_t)jnum(c, "uid", 0), req, k, out, sizeof(out));
        } else if (cmd == PROTO_ANCS_CMD_GET_APP_ATTRS) {
            uint8_t ids[PROTO_ANCS_MAX_ATTRS];
            size_t k = 0;
            const cJSON *a;
            cJSON_ArrayForEach(a, attrs) { ids[k++] = (uint8_t)jnum(a, "id", 0); }
            const char *app = jstr(c, "app_id");
            n = proto_ancs_build_get_app_attrs((const uint8_t *)app, strlen(app), ids, k, out, sizeof(out));
        } else {
            n = proto_ancs_build_perform_action((uint32_t)jnum(c, "uid", 0), (uint8_t)jnum(c, "action_id", 0), out,
                                                sizeof(out));
        }
        if (n != wl || memcmp(out, want, n) != 0) {
            CASE_FAIL(jstr(c, "name"), "request bytes differ");
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

static void test_ancs_data_source(void)
{
    cJSON *doc = load_vectors("ancs.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "data_source")) {
        const char *name = jstr(c, "name");
        const cJSON *req = cJSON_GetObjectItemCaseSensitive(c, "request");
        uint8_t ids[PROTO_ANCS_MAX_ATTRS];
        size_t nids = 0;
        const cJSON *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(req, "attribute_ids")) { ids[nids++] = (uint8_t)a->valuedouble; }
        size_t cap = (size_t)jnum(req, "capacity", 0);
        uint8_t *buf = malloc(cap);
        proto_ancs_ds_t ds;
        const char *app = jstr(req, "app_id");
        if (jnum(req, "command", 0) == PROTO_ANCS_CMD_GET_NOTIF_ATTRS) {
            proto_ancs_ds_init_notif(&ds, (uint32_t)jnum(req, "uid", 0), ids, nids, buf, cap);
        } else {
            proto_ancs_ds_init_app(&ds, (const uint8_t *)app, strlen(app), ids, nids, buf, cap);
        }
        const char *result = "NEED_MORE";
        int at = -1, i = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(c, "fragments")) {
            uint8_t frag[BUF];
            size_t fl = hex_decode(f->valuestring, frag, sizeof(frag));
            bool complete;
            proto_status_t st = proto_ancs_ds_feed(&ds, frag, fl, &complete);
            if (st != PROTO_OK) {
                result = proto_status_name(st);
                at = i;
                break;
            }
            if (complete && at < 0) {
                result = "OK";
                at = i;
            }
            i++;
        }
        if (strcmp(result, jstr(c, "expect")) != 0 || at != (int)jnum(c, "at_fragment", -1)) {
            CASE_FAIL(name, "result %s at %d", result, at);
        } else if (strcmp(result, "OK") == 0) {
            const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(c, "attributes");
            const cJSON *e;
            cJSON_ArrayForEach(e, attrs) {
                const uint8_t *v;
                uint16_t vl;
                uint8_t want[BUF];
                size_t wl = hex_decode(e->valuestring, want, sizeof(want));
                if (!proto_ancs_ds_attr(&ds, (uint8_t)atoi(e->string), &v, &vl) || vl != wl || memcmp(v, want, wl)) {
                    CASE_FAIL(name, "attribute %s differs", e->string);
                }
            }
        }
        free(buf);
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

static void test_ancs_dates(void)
{
    cJSON *doc = load_vectors("ancs.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "dates")) {
        const char *in = jstr(c, "input");
        int64_t t = -1;
        proto_status_t st = proto_ancs_parse_date((const uint8_t *)in, strlen(in), &t);
        if (st != status_by_name(jstr(c, "expect")) ||
            (st == PROTO_OK && t != (int64_t)jnum(c, "local_epoch_s", -1))) {
            CASE_FAIL(jstr(c, "name"), "status %s value %lld", proto_status_name(st), (long long)t);
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- crc32.json */

static void test_crc32(void)
{
    cJSON *doc = load_vectors("crc32.json");
    const cJSON *c;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        uint8_t d[BUF];
        size_t n = hex_decode(jstr(c, "hex"), d, sizeof(d));
        uint32_t want = (uint32_t)strtoul(jstr(c, "crc32"), NULL, 16);
        uint32_t whole = proto_crc32_update(0, d, n);
        uint32_t split = proto_crc32_update(proto_crc32_update(0, d, n / 3), d + n / 3, n - n / 3);
        if (whole != want || split != want) {
            CASE_FAIL(jstr(c, "name"), "crc %08x / %08x", whole, split);
        }
    }
    cJSON_Delete(doc);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- transfer.json */

static void test_transfer_scenarios(void)
{
    cJSON *doc = load_vectors("transfer.json");
    const cJSON *sc;
    int n_scenarios = 0, n_steps = 0;
    cJSON_ArrayForEach(sc, cases_of(doc, "scenarios")) {
        const char *name = jstr(sc, "name");
        cJSON *board = json_merge(cases_of(doc, "board"), cases_of(sc, "board"));
        xboard_t *b = xboard_new(board);
        cJSON_Delete(board);
        int i = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, cases_of(sc, "steps")) {
            const char *op = jstr(st, "op");
            n_steps++;
            if (strcmp(op, "disconnect") == 0) {
                xfer_on_disconnect(&b->x);
            } else if (strcmp(op, "reboot") == 0) {
                xboard_reboot(b);
            } else if (strcmp(op, "set_battery") == 0) {
                xboard_set_battery(b, cases_of(st, "battery"));
            } else if (strcmp(op, "request") == 0) {
                uint8_t msg[BUF];
                size_t len = hex_decode(jstr(st, "hex"), msg, sizeof(msg));
                xboard_request(b, msg, len);
                const cJSON *exp = cases_of(st, "expect");
                bool same = b->n_out == cJSON_GetArraySize(exp);
                for (int k = 0; same && k < b->n_out; k++) {
                    char hex[2 * PROTO_LIMIT_MAX_MESSAGE_TRANSFER + 1];
                    hex_encode(b->out[k], b->out_len[k], hex);
                    same = strcmp(hex, jstr(cJSON_GetArrayItem(exp, k), "hex")) == 0;
                }
                if (!same) {
                    char hex[2 * PROTO_LIMIT_MAX_MESSAGE_TRANSFER + 1] = "";
                    if (b->n_out) {
                        hex_encode(b->out[0], b->out_len[0], hex);
                    }
                    CASE_FAIL(name, "step %d: %d messages (first %s), want %d", i, b->n_out, hex,
                              cJSON_GetArraySize(exp));
                    break;
                }
            } else {
                CASE_FAIL(name, "unknown op %s", op);
            }
            i++;
        }
        xboard_free(b);
        n_scenarios++;
    }
    cJSON_Delete(doc);
    printf("transfer: %d scenarios, %d steps\n", n_scenarios, n_steps);
    TEST_ASSERT_GREATER_THAN(20, n_scenarios);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- dispatch.json */

static uint8_t chr_by_name(const char *s)
{
    return strcmp(s, "config") == 0 ? CORE_CHR_CONFIG
         : strcmp(s, "notification") == 0 ? CORE_CHR_NOTIFICATION
         : strcmp(s, "transfer") == 0 ? CORE_CHR_TRANSFER : 0;
}

static void test_dispatch(void)
{
    cJSON *doc = load_vectors("dispatch.json");
    const cJSON *c;
    int n_cases = 0;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        const char *name = jstr(c, "name");
        xcore_t *x = xcore_new();
        core_disconnect(&x->core);
        core_connect(&x->core, (uint16_t)jnum(c, "mtu", 517), cJSON_IsTrue(cases_of(c, "secure")));
        int i = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, cases_of(c, "steps")) {
            const cJSON *wr = cases_of(st, "write");
            uint8_t seg[BUF];
            size_t len = hex_decode(jstr(wr, "hex"), seg, sizeof(seg));
            x->n_out = 0;
            core_write(&x->core, chr_by_name(jstr(wr, "char")), seg, len);
            const cJSON *exp = cases_of(st, "expect");
            bool same = x->n_out == cJSON_GetArraySize(exp);
            for (int k = 0; same && k < x->n_out; k++) {
                const cJSON *e = cJSON_GetArrayItem(exp, k);
                char hex[2 * CORE_MTU_MAX + 1];
                hex_encode(x->out[k].v, x->out[k].len, hex);
                same = x->out[k].chr == chr_by_name(jstr(e, "char")) && strcmp(hex, jstr(e, "hex")) == 0;
            }
            if (!same) {
                char hex[2 * CORE_MTU_MAX + 1] = "";
                if (x->n_out) {
                    hex_encode(x->out[0].v, x->out[0].len, hex);
                }
                CASE_FAIL(name, "step %d: %d notifications (first %s), want %d", i, x->n_out, hex,
                          cJSON_GetArraySize(exp));
            }
            i++;
        }
        xcore_free(x);
        n_cases++;
    }
    cJSON_Delete(doc);
    TEST_ASSERT_GREATER_THAN(15, n_cases);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- wifi.json */

static void test_wifi_set(void)
{
    cJSON *doc = load_vectors("wifi.json");
    xcore_t *x = xcore_new();
    const cJSON *c;
    int n_cases = 0;
    cJSON_ArrayForEach(c, cases_of(doc, "cases")) {
        const char *name = jstr(c, "name");
        bool has_wifi = cJSON_IsTrue(cases_of(c, "has_wifi"));
        x->core.p.features = has_wifi ? x->core.p.features | PROTO_FEATURES_WIFI
                                      : x->core.p.features & ~(uint32_t)PROTO_FEATURES_WIFI;
        uint8_t req[BUF];
        size_t len = hex_decode(jstr(c, "hex"), req, sizeof(req));
        xcore_send(x, CORE_CHR_CONFIG, req, len);
        xcore_msg_t m[4];
        int n = xcore_messages(x, m, 4);
        uint8_t st = 0xFF;
        if (n < 1 || m[0].m[1] != PROTO_MSG_WIFI_SET_RSP ||
            !proto_get_u8(m[0].m + PROTO_HEADER_LEN, m[0].len - PROTO_HEADER_LEN, PROTO_TAG_WIFI_SET_RSP_STATUS, &st) ||
            strcmp(proto_status_name(st), jstr(c, "expect")) != 0) {
            CASE_FAIL(name, "%d messages, status %u, want %s", n, st, jstr(c, "expect"));
        }
        const cJSON *stored = cases_of(c, "stored_ssid");
        if (stored) {
            const char *want = cJSON_IsString(stored) ? stored->valuestring : "";
            const core_status_t *cs = &x->core.status;
            if (cs->wifi_ssid_len != strlen(want) || memcmp(cs->wifi_ssid, want, cs->wifi_ssid_len) != 0 ||
                strcmp(x->wifi_ssid, want) != 0 ||
                cs->wifi_state != (*want ? PROTO_WIFI_STATE_IDLE : PROTO_WIFI_STATE_UNCONFIGURED)) {
                CASE_FAIL(name, "stored ssid '%s' (status %u bytes, state %u), want '%s'", x->wifi_ssid,
                          cs->wifi_ssid_len, cs->wifi_state, want);
            }
        }
        const cJSON *saved = cases_of(c, "saved_ssids");
        if (saved) {
            const int n_saved = cJSON_GetArraySize(saved);
            if ((int)x->wifi_profile_count != n_saved) {
                CASE_FAIL(name, "%u saved networks, want %d", x->wifi_profile_count, n_saved);
            }
            for (int j = 0; j < n_saved && j < (int)x->wifi_profile_count; j++) {
                const cJSON *want = cJSON_GetArrayItem(saved, j);
                if (!cJSON_IsString(want) || strcmp(x->wifi_profiles[j], want->valuestring) != 0) {
                    CASE_FAIL(name, "saved network %d is '%s'", j, x->wifi_profiles[j]);
                }
            }
        }
        /* the password never leaves the board */
        for (int i = 0; *x->wifi_pw && i < n; i++) {
            size_t pl = strlen(x->wifi_pw);
            for (size_t k = 0; k + pl <= m[i].len; k++) {
                if (memcmp(m[i].m + k, x->wifi_pw, pl) == 0) {
                    CASE_FAIL(name, "password echoed in message %d", i);
                }
            }
        }
        n_cases++;
    }
    xcore_free(x);
    cJSON_Delete(doc);
    TEST_ASSERT_GREATER_THAN(10, n_cases);
    FINISH_CASES();
}

/* ---------------------------------------------------------------- app_icons.json */

static void test_app_icon_ids(void)
{
    cJSON *doc = load_vectors("app_icons.json");
    const cJSON *c;
    int n_cases = 0;
    cJSON_ArrayForEach(c, cases_of(doc, "ids")) {
        uint8_t app_id[64];
        size_t n = hex_decode(jstr(c, "app_id_hex"), app_id, sizeof(app_id));
        char got[XFER_APP_ICON_ID_LEN + 1];
        xfer_app_icon_id((const char *)app_id, n, got);
        if (n == SIZE_MAX || strlen(jstr(c, "app_id")) != n || memcmp(jstr(c, "app_id"), app_id, n) != 0 ||
            strcmp(got, jstr(c, "icon_id")) != 0) {
            CASE_FAIL(jstr(c, "name"), "icon id %s", got);
        }
        n_cases++;
    }
    cJSON_Delete(doc);
    TEST_ASSERT_GREATER_THAN(5, n_cases);
    FINISH_CASES();
}

/* The board deletes an app's icon when the app leaves the seen list (protocol.md 6.4, since 0.12.0). */
static void test_icon_removal(void)
{
    cJSON *doc = load_vectors("app_icons.json");
    const cJSON *rem = cases_of(doc, "removal"), *c;
    int n_cases = 0;
    cJSON_ArrayForEach(c, cases_of(rem, "cases")) {
        xcore_t *x = xcore_new();
        TEST_ASSERT_EQUAL_INT((int)jnum(rem, "seen_apps", 0), (int)(sizeof(x->apps) / sizeof(x->apps[0])));
        int req = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, cases_of(c, "steps")) {
            const char *op = jstr(st, "op"), *app = jstr(st, "app_id");
            char json[1024];
            req++;
            if (strcmp(op, "seen") == 0) {
                snprintf(json, sizeof(json), "{\"type\":\"NOTIF_POST\",\"req_id\":%d,\"fields\":{\"key\":\"k%d\","
                         "\"app_id\":\"%s\",\"title\":\"t\"}}", req, req, app);
                xcore_send_json(x, CORE_CHR_NOTIFICATION, json);
            } else if (strcmp(op, "icon") == 0) {
                char id[XFER_APP_ICON_ID_LEN + 1], name[BLOB_NAME_MAX + 1];
                xfer_app_icon_id(app, strlen(app), id);
                xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, id, XFER_APP_ICON_ID_LEN, name);
                TEST_ASSERT_EQUAL_INT(BLOB_OK, x->blob.iface.write(x->blob.iface.ctx, name, 0, "icon", 4));
            } else if (strcmp(op, "policy") == 0) {
                snprintf(json, sizeof(json), "{\"type\":\"APPS_SET_POLICY\",\"req_id\":%d,\"fields\":{\"entry\":["
                         "{\"app_id\":\"%s\",\"policy\":%d}]}}", req, app, (int)jnum(st, "policy", 0));
                xcore_send_json(x, CORE_CHR_CONFIG, json);
            } else {
                size_t n = (size_t)snprintf(json, sizeof(json), "{\"type\":\"APPS_FORGET\",\"req_id\":%d,\"fields\":{", req);
                const cJSON *ids = cJSON_GetObjectItemCaseSensitive(st, "app_ids"), *id;
                n += (size_t)snprintf(json + n, sizeof(json) - n, cJSON_GetArraySize(ids) ? "\"app_id\":[" : "");
                int k = 0;
                cJSON_ArrayForEach(id, ids) {
                    n += (size_t)snprintf(json + n, sizeof(json) - n, "%s\"%s\"", k++ ? "," : "", id->valuestring);
                }
                snprintf(json + n, sizeof(json) - n, "%s}}", cJSON_GetArraySize(ids) ? "]" : "");
                xcore_send_json(x, CORE_CHR_CONFIG, json);
            }
        }
        /* the installed icons are exactly `icons_after` */
        const cJSON *want = cases_of(c, "icons_after"), *w;
        char name[BLOB_NAME_MAX + 1];
        uint32_t size, have = 0;
        while (x->blob.iface.list(x->blob.iface.ctx, have, name, sizeof(name), &size) == BLOB_OK) {
            have++;
        }
        if ((int)have != cJSON_GetArraySize(want)) {
            CASE_FAIL(jstr(c, "name"), "%u icons installed, want %d", (unsigned)have, cJSON_GetArraySize(want));
        }
        cJSON_ArrayForEach(w, want) {
            char file[BLOB_NAME_MAX + 1];
            xfer_asset_blob_name(PROTO_ASSET_TYPE_APP_ICON, w->valuestring, XFER_APP_ICON_ID_LEN, file);
            if (x->blob.iface.stat(x->blob.iface.ctx, file, &size) != BLOB_OK) {
                CASE_FAIL(jstr(c, "name"), "icon %s is missing", w->valuestring);
            }
        }
        /* and the seen list is exactly `apps_after` */
        const cJSON *apps = cases_of(c, "apps_after"), *a;
        int listed = 0;
        for (size_t i = 0; i < sizeof(x->apps) / sizeof(x->apps[0]); i++) {
            listed += x->apps[i].used;
        }
        if (listed != cJSON_GetArraySize(apps)) {
            CASE_FAIL(jstr(c, "name"), "%d apps listed, want %d", listed, cJSON_GetArraySize(apps));
        }
        cJSON_ArrayForEach(a, apps) {
            bool found = false;
            for (size_t i = 0; i < sizeof(x->apps) / sizeof(x->apps[0]); i++) {
                found |= x->apps[i].used && strcmp(x->apps[i].id, a->valuestring) == 0;
            }
            if (!found) {
                CASE_FAIL(jstr(c, "name"), "app %s is not listed", a->valuestring);
            }
        }
        xcore_free(x);
        n_cases++;
    }
    cJSON_Delete(doc);
    TEST_ASSERT_GREATER_THAN(8, n_cases);
    FINISH_CASES();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_messages);
    RUN_TEST(test_capabilities_fit_att_value);
    RUN_TEST(test_segments_split);
    RUN_TEST(test_segments_reassemble);
    RUN_TEST(test_config_values);
    RUN_TEST(test_text);
    RUN_TEST(test_ancs_notification_source);
    RUN_TEST(test_ancs_requests);
    RUN_TEST(test_ancs_data_source);
    RUN_TEST(test_ancs_dates);
    RUN_TEST(test_crc32);
    RUN_TEST(test_transfer_scenarios);
    RUN_TEST(test_dispatch);
    RUN_TEST(test_wifi_set);
    RUN_TEST(test_app_icon_ids);
    RUN_TEST(test_icon_removal);
    return UNITY_END();
}
