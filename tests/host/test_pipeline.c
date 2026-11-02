/* notification_pipeline: normalisation, filters, quiet hours, store, ANCS, seen apps, messages. */
#include <stdlib.h>
#include <string.h>

#include "epager/npipe.h"
#include "test_support.h"
#include "unity.h"

#define SLOTS 8
#define APPS 4

static kv_mem_t cfg_kv, apps_kv;
static cfg_store_t cfg;
static npipe_t np;
static npipe_slot_t slots[SLOTS];
static npipe_app_t apps[APPS];
static uint32_t clock_now;
static int32_t power;

/* event log */
static struct {
    uint32_t id;
    uint8_t kind;
} events[64];
static int n_events, n_seen, n_cfg_events;
static npipe_app_t last_seen_app;

static uint32_t now_utc(void *ctx)
{
    (void)ctx;
    return clock_now;
}
static void on_event(void *ctx, const npipe_notif_t *n)
{
    (void)ctx;
    TEST_ASSERT_LESS_THAN(64, n_events);
    events[n_events].id = n->id;
    events[n_events++].kind = n->kind;
}
static void on_app_seen(void *ctx, const npipe_app_t *a)
{
    (void)ctx;
    n_seen++;
    last_seen_app = *a;
}
static void on_cfg_change(void *ctx, uint32_t rev, const uint16_t *keys, size_t n)
{
    (void)ctx, (void)rev, (void)keys, (void)n;
    n_cfg_events++;
}

static void boot_pipeline(size_t n_slots, size_t n_apps)
{
    npipe_params_t p = {.cfg = &cfg, .kv = &apps_kv.iface, .slots = slots, .n_slots = n_slots, .apps = apps,
                        .n_apps = n_apps, .now_utc = now_utc, .on_event = on_event, .on_app_seen = on_app_seen};
    npipe_init(&np, &p);
}

void setUp(void)
{
    kv_mem_init(&cfg_kv);
    kv_mem_init(&apps_kv);
    cfg_params_t cp = {.kv = &cfg_kv.iface, .variant = PROTO_VARIANT_PAPER, .on_change = on_cfg_change};
    cfg_init(&cfg, &cp);
    clock_now = 1791700215; /* 2026-10-11 03:10:15 UTC */
    n_events = n_seen = n_cfg_events = 0;
    boot_pipeline(SLOTS, APPS);
}
void tearDown(void) {}

static npipe_str_t S(const char *s) { return (npipe_str_t){(const uint8_t *)s, s ? strlen(s) : 0}; }

static uint32_t post(const char *key, const char *app, uint8_t category, uint8_t priority, uint8_t flags, bool *shown)
{
    npipe_android_t a = {.key = S(key), .app_id = S(app), .title = S("t"), .category = category,
                         .priority = priority, .flags = flags};
    uint32_t id = 0;
    bool sh = false;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_post(&np, &a, &id, &sh));
    if (shown) {
        *shown = sh;
    }
    return id;
}

static bool shown_simple(const char *key) /* app "a", category social, normal priority */
{
    bool sh;
    post(key, "a", PROTO_CATEGORY_SOCIAL, PROTO_PRIORITY_NORMAL, 0, &sh);
    return sh;
}

static void set_cfg(uint16_t key, int32_t v)
{
    uint8_t b[4];
    size_t len = 1;
    const proto_cfg_key_desc_t *k = proto_cfg_key(key);
    if (k->type == PROTO_FT_U16 || k->type == PROTO_FT_I16) {
        proto_wr16(b, (uint16_t)v);
        len = 2;
    } else {
        b[0] = (uint8_t)v;
    }
    cfg_entry_t e = {key, b, len};
    TEST_ASSERT_EQUAL_INT(PROTO_OK, cfg_set(&cfg, &e, 1, NULL, NULL));
}

#define ASSERT_EVENT(i, kind_, id_)                         \
    do {                                                    \
        TEST_ASSERT_EQUAL_UINT8((kind_), events[(i)].kind); \
        TEST_ASSERT_EQUAL_UINT32((id_), events[(i)].id);    \
    } while (0)

/* ---------------------------------------------------------------- Android add / modify / remove */

static void test_android_lifecycle(void)
{
    npipe_android_t a = {.key = S("0|com.zing.zalo|1"), .app_id = S("com.zing.zalo"), .app_name = S("Zalo"),
                         .title = S("Nguye\xcc\x82\xcc\x83n Va\xcc\x86n An"), /* NFD Vietnamese */
                         .subtitle = S("Nh\xc3\xb3m"), .body = S("T\xe1\xbb\x91i nay? \xf0\x9f\x98\x80 \xe4\xbd\xa0"),
                         .category = PROTO_CATEGORY_SOCIAL, .priority = PROTO_PRIORITY_HIGH,
                         .flags = PROTO_NOTIF_FLAGS_POSITIVE_ACTION | 0xE0};
    uint32_t id;
    bool shown;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_post(&np, &a, &id, &shown));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    TEST_ASSERT_TRUE(shown);
    TEST_ASSERT_EQUAL_INT(1, n_events);
    ASSERT_EVENT(0, NPIPE_ADDED, 1);
    const npipe_notif_t *n = npipe_get(&np, 1);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("Nguy\xe1\xbb\x85n V\xc4\x83n An", n->title); /* NFC */
    TEST_ASSERT_EQUAL_STRING("T\xe1\xbb\x91i nay? \xf0\x9f\x98\x80 \xe2\x96\xa1", n->body);  /* emoji kept, CJK -> placeholder */
    TEST_ASSERT_EQUAL_STRING("com.zing.zalo", n->app_id);
    TEST_ASSERT_EQUAL_STRING("Zalo", n->app_name);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_FLAGS_POSITIVE_ACTION, n->flags); /* unknown bits dropped */
    TEST_ASSERT_EQUAL_UINT32(clock_now, n->timestamp);                      /* 0 = now */
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANDROID, n->source);

    a.title = S("Updated");
    a.timestamp = 1000;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_post(&np, &a, &id, &shown));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    ASSERT_EVENT(1, NPIPE_MODIFIED, 1);
    TEST_ASSERT_EQUAL_STRING("Updated", npipe_get(&np, 1)->title);
    TEST_ASSERT_EQUAL_UINT32(1000, npipe_get(&np, 1)->timestamp);

    TEST_ASSERT_EQUAL_INT(PROTO_NOT_FOUND, npipe_android_remove(&np, (const uint8_t *)"nope", 4));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_remove(&np, a.key.p, a.key.len));
    ASSERT_EVENT(2, NPIPE_REMOVED, 1);
    TEST_ASSERT_NULL(npipe_get(&np, 1));
    TEST_ASSERT_EQUAL_UINT(0, npipe_count(&np, false));

    a.key = S("");
    TEST_ASSERT_EQUAL_INT(PROTO_MALFORMED, npipe_android_post(&np, &a, &id, &shown));
}

static void test_dismiss(void)
{
    uint32_t a = post("k-a", "com.zing.zalo", PROTO_CATEGORY_SOCIAL, PROTO_PRIORITY_NORMAL, 0, NULL);
    npipe_dismissed_t d;
    TEST_ASSERT_EQUAL_INT(PROTO_NOT_FOUND, npipe_dismiss(&np, a + 100, &d));
    n_events = 0;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_dismiss(&np, a, &d));
    TEST_ASSERT_EQUAL_UINT32(a, d.id);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANDROID, d.source);
    TEST_ASSERT_EQUAL_UINT8(3, d.key_len);
    TEST_ASSERT_EQUAL_MEMORY("k-a", d.key, 3);
    ASSERT_EVENT(0, NPIPE_REMOVED, a);
    TEST_ASSERT_NULL(npipe_get(&np, a));
    TEST_ASSERT_EQUAL_INT(PROTO_NOT_FOUND, npipe_dismiss(&np, a, &d));
    /* the app's NOTIF_REMOVE for the same key afterwards is NOT_FOUND (protocol.md 6.5) */
    TEST_ASSERT_EQUAL_INT(PROTO_NOT_FOUND, npipe_android_remove(&np, (const uint8_t *)"k-a", 3));

    const char *id = "com.apple.MobileSMS";
    npipe_ancs_t in = {.ns = {.event_id = 0, .category_id = PROTO_CATEGORY_SOCIAL, .uid = 77},
                       .app_id = {(const uint8_t *)id, strlen(id)}, .title = {(const uint8_t *)"T", 1}};
    npipe_ancs_event(&np, &in);
    const npipe_notif_t *list[4];
    TEST_ASSERT_EQUAL_UINT(1, npipe_list(&np, list, 4));
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_dismiss(&np, list[0]->id, &d));
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANCS, d.source);
    TEST_ASSERT_EQUAL_UINT32(77, d.ancs_uid);
    TEST_ASSERT_EQUAL_UINT8(0, d.key_len); /* no key for ANCS */
}

static void test_unknown_enums_fall_back(void)
{
    uint32_t id = post("k", "a", 200, 9, 0, NULL);
    const npipe_notif_t *n = npipe_get(&np, id);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CATEGORY_OTHER, n->category);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_NORMAL, n->priority);
}

static void test_long_text_truncated_on_board(void)
{
    static char body[NPIPE_BODY_MAX + 1];
    memset(body, 0xFF, NPIPE_BODY_MAX); /* invalid bytes: each would become a placeholder run */
    for (size_t i = 0; i < NPIPE_BODY_MAX; i += 2) {
        body[i] = 'a';
    }
    npipe_android_t a = {.key = S("k"), .app_id = S("a"), .body = S(body)};
    uint32_t id;
    bool shown;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_post(&np, &a, &id, &shown));
    const npipe_notif_t *n = npipe_get(&np, id);
    TEST_ASSERT_LESS_OR_EQUAL(NPIPE_BODY_MAX, n->body_len);
    TEST_ASSERT_EQUAL_UINT(strlen(n->body), n->body_len);
    TEST_ASSERT_EQUAL_STRING("\xe2\x80\xa6", n->body + n->body_len - 3); /* ellipsis */
    TEST_ASSERT_TRUE(proto_utf8_valid((const uint8_t *)n->body, n->body_len));
}

static void test_clear_only_android(void)
{
    post("k1", "a", 0, 1, 0, NULL);
    post("k2", "b", 0, 1, 0, NULL);
    npipe_ancs_t in = {.ns = {PROTO_ANCS_EVENT_ADDED, 0, PROTO_CATEGORY_EMAIL, 1, 77}, .app_id = S("com.apple.mail")};
    npipe_ancs_event(&np, &in);
    TEST_ASSERT_EQUAL_UINT(3, npipe_count(&np, true));
    npipe_android_clear(&np);
    TEST_ASSERT_EQUAL_UINT(1, npipe_count(&np, true));
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANCS, npipe_get(&np, 3)->source);
}

/* ---------------------------------------------------------------- filters */

static void test_filters(void)
{
    bool sh;
    TEST_ASSERT_TRUE(shown_simple("k"));

    set_cfg(PROTO_CFG_NOTIF_CATEGORY_MASK, 0x0FFF & ~(1 << PROTO_CATEGORY_SOCIAL));
    TEST_ASSERT_FALSE(shown_simple("k"));
    post("k2", "a", 200, PROTO_PRIORITY_NORMAL, 0, &sh); /* unknown -> other, still allowed */
    TEST_ASSERT_TRUE(sh);
    set_cfg(PROTO_CFG_NOTIF_CATEGORY_MASK, 0x0FFF);

    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_HIGH);
    TEST_ASSERT_FALSE(shown_simple("k"));
    post("k", "a", 0, PROTO_PRIORITY_HIGH, 0, &sh);
    TEST_ASSERT_TRUE(sh);
    post("k", "a", 0, PROTO_PRIORITY_URGENT, 0, &sh);
    TEST_ASSERT_TRUE(sh);
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_LOW);

    post("k", "a", 0, PROTO_PRIORITY_LOW, PROTO_NOTIF_FLAGS_SILENT, &sh);
    TEST_ASSERT_TRUE(sh);
    set_cfg(PROTO_CFG_NOTIF_SHOW_SILENT, 0);
    post("k", "a", 0, PROTO_PRIORITY_LOW, PROTO_NOTIF_FLAGS_SILENT, &sh);
    TEST_ASSERT_FALSE(sh);
    post("k", "a", 0, PROTO_PRIORITY_LOW, 0, &sh);
    TEST_ASSERT_TRUE(sh);
}

static void test_app_policies(void)
{
    bool changed;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_app_set_policy(&np, (const uint8_t *)"a", 1, PROTO_APP_POLICY_BLOCK, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_FALSE(shown_simple("k"));
    bool sh;
    post("k2", "b", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    TEST_ASSERT_TRUE(sh);

    set_cfg(PROTO_CFG_NOTIF_DEFAULT_APP_POLICY, PROTO_DEFAULT_POLICY_BLOCK);
    post("k2", "b", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    TEST_ASSERT_FALSE(sh);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_app_set_policy(&np, (const uint8_t *)"b", 1, PROTO_APP_POLICY_ALLOW, &changed));
    post("k2", "b", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    TEST_ASSERT_TRUE(sh);
    /* allow does not bypass the other filters */
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_URGENT);
    post("k2", "b", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    TEST_ASSERT_FALSE(sh);

    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_app_set_policy(&np, (const uint8_t *)"b", 1, PROTO_APP_POLICY_ALLOW, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_app_set_policy(&np, (const uint8_t *)"b", 1, 77, &changed)); /* -> default */
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT8(PROTO_APP_POLICY_DEFAULT, npipe_app_find(&np, (const uint8_t *)"b", 1)->policy);
}

static uint32_t at(int hh, int mm) { return 1791676800u + (uint32_t)(hh * 3600 + mm * 60); } /* 2026-10-11 UTC */

static void test_quiet_hours(void)
{
    const struct {
        int start, end, tz, hh, mm;
        bool quiet;
    } cases[] = {
        {1320, 420, 0, 23, 30, true},  /* 22:00-07:00 spans midnight */
        {1320, 420, 0, 22, 0, true},
        {1320, 420, 0, 0, 0, true},
        {1320, 420, 0, 6, 59, true},
        {1320, 420, 0, 7, 0, false},   /* end is exclusive */
        {1320, 420, 0, 21, 59, false},
        {1320, 420, 0, 12, 0, false},
        {780, 840, 0, 13, 30, true},   /* 13:00-14:00 same day */
        {780, 840, 0, 14, 0, false},
        {780, 840, 0, 12, 59, false},
        {600, 600, 0, 10, 0, false},   /* start == end: empty */
        {1320, 420, 420, 16, 0, true}, /* 16:00 UTC = 23:00 at UTC+7 */
        {1320, 420, 420, 0, 30, false}, /* 07:30 local */
        {1320, 420, -300, 2, 0, false}, /* 02:00 UTC = 21:00 the day before at UTC-5 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        set_cfg(PROTO_CFG_QUIET_ENABLED, 1);
        set_cfg(PROTO_CFG_QUIET_START_MIN, cases[i].start);
        set_cfg(PROTO_CFG_QUIET_END_MIN, cases[i].end);
        set_cfg(PROTO_CFG_TIME_TZ_OFFSET_MIN, cases[i].tz);
        clock_now = at(cases[i].hh, cases[i].mm);
        char msg[32];
        snprintf(msg, sizeof(msg), "case %zu", i);
        TEST_ASSERT_EQUAL_MESSAGE(!cases[i].quiet, shown_simple("q"), msg);
    }
    set_cfg(PROTO_CFG_TIME_TZ_OFFSET_MIN, -180);
    clock_now = at(1, 30); /* 22:30 local the day before */
    TEST_ASSERT_FALSE(shown_simple("q"));
    clock_now = 0; /* clock not set: quiet hours cannot apply */
    TEST_ASSERT_TRUE(shown_simple("q"));
    clock_now = at(23, 0);
    set_cfg(PROTO_CFG_TIME_TZ_OFFSET_MIN, 0);
    set_cfg(PROTO_CFG_QUIET_ENABLED, 0);
    TEST_ASSERT_TRUE(shown_simple("q"));
}

static void test_modify_transitions(void)
{
    bool sh;
    uint32_t id = post("k", "a", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_HIGH);
    post("k", "a", PROTO_CATEGORY_SOCIAL, 1, 0, &sh); /* shown -> hidden */
    post("k", "a", PROTO_CATEGORY_SOCIAL, 1, 0, &sh); /* hidden -> hidden: no event */
    post("k", "a", PROTO_CATEGORY_SOCIAL, 2, 0, &sh); /* hidden -> shown */
    TEST_ASSERT_EQUAL_INT(3, n_events);
    ASSERT_EVENT(0, NPIPE_ADDED, id);
    ASSERT_EVENT(1, NPIPE_REMOVED, id);
    ASSERT_EVENT(2, NPIPE_ADDED, id);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_remove(&np, (const uint8_t *)"k", 1));
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_URGENT);
    id = post("h", "a", PROTO_CATEGORY_SOCIAL, 1, 0, &sh); /* hidden: stored, no event */
    TEST_ASSERT_FALSE(sh);
    TEST_ASSERT_EQUAL_INT(4, n_events);
    TEST_ASSERT_EQUAL_UINT8(NPIPE_ADDED, npipe_get(&np, id)->kind);
    post("h", "a", PROTO_CATEGORY_SOCIAL, 1, 0, &sh);
    TEST_ASSERT_EQUAL_UINT8(NPIPE_MODIFIED, npipe_get(&np, id)->kind);
    TEST_ASSERT_EQUAL_INT(PROTO_OK, npipe_android_remove(&np, (const uint8_t *)"h", 1)); /* hidden: no event */
    TEST_ASSERT_EQUAL_INT(4, n_events);
}

/* ---------------------------------------------------------------- store */

static void test_store_eviction_and_listing(void)
{
    boot_pipeline(3, APPS);
    post("a", "x", 0, 1, 0, NULL); /* id 1 */
    post("b", "x", 0, 1, 0, NULL); /* id 2 */
    post("c", "x", 0, 1, 0, NULL); /* id 3 */
    post("a", "x", 0, 1, 0, NULL); /* update makes id 1 the newest */
    const npipe_notif_t *list[4];
    TEST_ASSERT_EQUAL_UINT(3, npipe_list(&np, list, 4));
    TEST_ASSERT_EQUAL_UINT32(1, list[0]->id);
    TEST_ASSERT_EQUAL_UINT32(3, list[1]->id);
    TEST_ASSERT_EQUAL_UINT32(2, list[2]->id);
    TEST_ASSERT_EQUAL_UINT(2, npipe_list(&np, list, 2));

    n_events = 0;
    post("d", "x", 0, 1, 0, NULL); /* full: the oldest (id 2) goes */
    ASSERT_EVENT(0, NPIPE_REMOVED, 2);
    ASSERT_EVENT(1, NPIPE_ADDED, 4);
    TEST_ASSERT_NULL(npipe_get(&np, 2));

    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_HIGH);
    post("e", "x", 0, 1, 0, NULL); /* hidden id 5 evicts id 3 */
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_LOW);
    n_events = 0;
    post("f", "x", 0, 1, 0, NULL); /* the hidden one goes first, although it is newer */
    TEST_ASSERT_EQUAL_INT(1, n_events);
    TEST_ASSERT_NULL(npipe_get(&np, 5));
    TEST_ASSERT_NOT_NULL(npipe_get(&np, 1));
    TEST_ASSERT_EQUAL_UINT(3, npipe_count(&np, true));
    TEST_ASSERT_NULL(npipe_get(&np, 999));
}

/* ---------------------------------------------------------------- ANCS */

static size_t ds_response(uint8_t *out, uint32_t uid, const char *app, const char *title, const char *date)
{
    const struct {
        uint8_t id;
        const char *v;
    } attrs[] = {{PROTO_ANCS_ATTR_APP_ID, app}, {PROTO_ANCS_ATTR_TITLE, title}, {PROTO_ANCS_ATTR_SUBTITLE, ""},
                 {PROTO_ANCS_ATTR_MESSAGE, "Body"}, {PROTO_ANCS_ATTR_DATE, date}};
    size_t n = 0;
    out[n++] = PROTO_ANCS_CMD_GET_NOTIF_ATTRS;
    proto_wr32(out + n, uid);
    n += 4;
    for (size_t i = 0; i < 5; i++) {
        size_t l = strlen(attrs[i].v);
        out[n++] = attrs[i].id;
        proto_wr16(out + n, (uint16_t)l);
        memcpy(out + n + 2, attrs[i].v, l);
        n += 2 + l;
    }
    return n;
}

static uint32_t ancs_add(uint8_t event, uint8_t flags, uint8_t category, uint32_t uid, const char *app, const char *date)
{
    uint8_t rsp[512], buf[512];
    size_t n = ds_response(rsp, uid, app, "Title", date);
    const uint8_t ids[] = {0, 1, 2, 3, 5};
    proto_ancs_ds_t ds;
    proto_ancs_ds_init_notif(&ds, uid, ids, 5, buf, sizeof(buf));
    bool complete;
    TEST_ASSERT_EQUAL_INT(PROTO_OK, proto_ancs_ds_feed(&ds, rsp, n, &complete));
    TEST_ASSERT_TRUE(complete);
    proto_ancs_ns_t ns = {event, flags, category, 1, uid};
    npipe_ancs_t in;
    npipe_ancs_from_ds(&in, &ns, &ds, S("Mail"));
    npipe_ancs_event(&np, &in);
    for (size_t i = 0; i < SLOTS; i++) {
        if (slots[i].used && slots[i].n.source == PROTO_NOTIF_SOURCE_ANCS && slots[i].ancs_uid == uid) {
            return slots[i].n.id;
        }
    }
    return 0;
}

static void test_ancs_mapping(void)
{
    set_cfg(PROTO_CFG_TIME_TZ_OFFSET_MIN, 420);
    uint32_t id = ancs_add(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_IMPORTANT | PROTO_ANCS_FLAG_PRE_EXISTING,
                           PROTO_CATEGORY_EMAIL, 10, "com.apple.mobilemail", "20261011T100000");
    const npipe_notif_t *n = npipe_get(&np, id);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_HIGH, n->priority);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_FLAGS_IMPORTANT | PROTO_NOTIF_FLAGS_PRE_EXISTING, n->flags);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CATEGORY_EMAIL, n->category);
    TEST_ASSERT_EQUAL_UINT32(at(3, 0), n->timestamp); /* 10:00 at UTC+7 */
    TEST_ASSERT_EQUAL_STRING("Title", n->title);
    TEST_ASSERT_EQUAL_STRING("Body", n->body);
    TEST_ASSERT_EQUAL_STRING("Mail", n->app_name);
    TEST_ASSERT_EQUAL_STRING("com.apple.mobilemail", n->app_id);

    id = ancs_add(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_IMPORTANT, PROTO_CATEGORY_INCOMING_CALL, 11, "c", "bad");
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_URGENT, npipe_get(&np, id)->priority);
    TEST_ASSERT_EQUAL_UINT32(clock_now, npipe_get(&np, id)->timestamp); /* invalid date -> now */
    id = ancs_add(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_SILENT, 42, 12, "c", "19991231T235959");
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_LOW, npipe_get(&np, id)->priority);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CATEGORY_OTHER, npipe_get(&np, id)->category);
    set_cfg(PROTO_CFG_TIME_TZ_OFFSET_MIN, 840);
    id = ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 0, 13, "c", "20000101T000000");
    TEST_ASSERT_EQUAL_UINT32(946684800u - 840u * 60u, npipe_get(&np, id)->timestamp);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_NORMAL, npipe_get(&np, id)->priority);
}

static void test_ancs_events_and_session_reset(void)
{
    uint32_t a = ancs_add(PROTO_ANCS_EVENT_MODIFIED, 0, 4, 100, "app.one", "20261011T100000"); /* unknown -> added */
    ASSERT_EVENT(0, NPIPE_ADDED, a);
    TEST_ASSERT_EQUAL_UINT32(a, ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 4, 100, "app.one", "20261011T100000"));
    ASSERT_EVENT(1, NPIPE_MODIFIED, a);
    uint32_t b = ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 4, 101, "app.two", "20261011T100000");
    post("k", "android.app", 0, 1, 0, NULL);
    TEST_ASSERT_EQUAL_INT(4, n_events);

    npipe_ancs_t rm = {.ns = {PROTO_ANCS_EVENT_REMOVED, 0, 0, 0, 999}};
    npipe_ancs_event(&np, &rm); /* unknown uid: ignored */
    rm.ns.event_id = 7;         /* reserved event id: ignored */
    rm.ns.uid = 100;
    npipe_ancs_event(&np, &rm);
    TEST_ASSERT_EQUAL_INT(4, n_events);
    rm.ns.event_id = PROTO_ANCS_EVENT_REMOVED;
    npipe_ancs_event(&np, &rm);
    ASSERT_EVENT(4, NPIPE_REMOVED, a);

    npipe_ancs_session_reset(&np);
    ASSERT_EVENT(5, NPIPE_REMOVED, b);
    TEST_ASSERT_EQUAL_UINT(1, npipe_count(&np, false)); /* the Android one stays */
}

static void test_ancs_app_id_sanitised(void)
{
    uint32_t id = ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 0, 1, "bad\xff", "x");
    TEST_ASSERT_EQUAL_STRING("unknown", npipe_get(&np, id)->app_id);
    id = ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 0, 2, "", "x");
    TEST_ASSERT_EQUAL_STRING("unknown", npipe_get(&np, id)->app_id);
    char long_id[80];
    memset(long_id, 'z', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    long_id[63] = '\xc3'; /* a 2-byte character straddling the 64-byte limit */
    long_id[64] = '\xa9';
    id = ancs_add(PROTO_ANCS_EVENT_ADDED, 0, 0, 3, long_id, "x");
    TEST_ASSERT_EQUAL_UINT(63, npipe_get(&np, id)->app_id_len);
}

/* ---------------------------------------------------------------- seen apps */

static void test_seen_apps_tracking_and_persistence(void)
{
    npipe_android_t a = {.key = S("k"), .app_id = S("com.zing.zalo"), .app_name = S("Zalo")};
    uint32_t id;
    bool sh;
    npipe_android_post(&np, &a, &id, &sh);
    TEST_ASSERT_EQUAL_INT(1, n_seen);
    TEST_ASSERT_EQUAL_STRING("com.zing.zalo", last_seen_app.id);
    TEST_ASSERT_EQUAL_STRING("Zalo", last_seen_app.name);
    TEST_ASSERT_EQUAL_UINT32(clock_now, last_seen_app.last_seen);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANDROID, last_seen_app.source);
    uint32_t writes = apps_kv.mutations;
    clock_now += 60;
    a.app_name = S("Zalo VN");
    npipe_android_post(&np, &a, &id, &sh);
    TEST_ASSERT_EQUAL_INT(1, n_seen);              /* only the first time */
    TEST_ASSERT_EQUAL_UINT32(writes, apps_kv.mutations); /* recency is not written per notification */
    npipe_apps_flush(&np);
    TEST_ASSERT_EQUAL_UINT32(writes + 1, apps_kv.mutations);
    npipe_apps_flush(&np);
    TEST_ASSERT_EQUAL_UINT32(writes + 1, apps_kv.mutations);

    bool changed;
    npipe_app_set_policy(&np, (const uint8_t *)"com.zing.zalo", 13, PROTO_APP_POLICY_BLOCK, &changed);
    boot_pipeline(SLOTS, APPS); /* reboot */
    const npipe_app_t *z = npipe_app_find(&np, (const uint8_t *)"com.zing.zalo", 13);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_EQUAL_UINT8(PROTO_APP_POLICY_BLOCK, z->policy);
    TEST_ASSERT_EQUAL_STRING("Zalo VN", z->name);
    TEST_ASSERT_EQUAL_UINT32(clock_now, z->last_seen);
    TEST_ASSERT_EQUAL_UINT(0, npipe_count(&np, false)); /* notifications are RAM only */
}

static void test_seen_apps_lru_keeps_explicit_policies(void)
{
    bool changed;
    npipe_app_set_policy(&np, (const uint8_t *)"keep", 4, PROTO_APP_POLICY_ALLOW, &changed);
    post("1", "old", 0, 1, 0, NULL);
    post("2", "mid", 0, 1, 0, NULL);
    post("3", "new", 0, 1, 0, NULL); /* full: keep, old, mid, new */
    post("4", "old", 0, 1, 0, NULL); /* old becomes recent */
    post("5", "fifth", 0, 1, 0, NULL); /* evicts mid, not keep (explicit) nor old (recent) */
    TEST_ASSERT_NULL(npipe_app_find(&np, (const uint8_t *)"mid", 3));
    TEST_ASSERT_NOT_NULL(npipe_app_find(&np, (const uint8_t *)"keep", 4));
    TEST_ASSERT_NOT_NULL(npipe_app_find(&np, (const uint8_t *)"old", 3));
    TEST_ASSERT_EQUAL_UINT(APPS, npipe_app_count(&np));

    /* full of explicit policies: new apps are not tracked but still filtered with the default */
    const char *ids[] = {"old", "new", "fifth"};
    for (int i = 0; i < 3; i++) {
        npipe_app_set_policy(&np, (const uint8_t *)ids[i], strlen(ids[i]), PROTO_APP_POLICY_BLOCK, &changed);
    }
    TEST_ASSERT_EQUAL_INT(PROTO_NO_SPACE, npipe_app_set_policy(&np, (const uint8_t *)"x", 1, 1, &changed));
    bool sh;
    n_seen = 0;
    post("6", "untracked", 0, 1, 0, &sh);
    TEST_ASSERT_TRUE(sh);
    TEST_ASSERT_EQUAL_INT(0, n_seen);
    TEST_ASSERT_NULL(npipe_app_find(&np, (const uint8_t *)"untracked", 9));

    TEST_ASSERT_TRUE(npipe_app_forget(&np, (const uint8_t *)"old", 3));
    TEST_ASSERT_FALSE(npipe_app_forget(&np, (const uint8_t *)"old", 3));
    TEST_ASSERT_EQUAL_UINT(3, npipe_app_count(&np));
    TEST_ASSERT_TRUE(npipe_app_forget(&np, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(0, npipe_app_count(&np));
    boot_pipeline(SLOTS, APPS);
    TEST_ASSERT_EQUAL_UINT(0, npipe_app_count(&np));
}

static void test_seen_apps_storage_failures(void)
{
    power = 0;
    apps_kv.power = &power;
    bool changed;
    TEST_ASSERT_EQUAL_INT(PROTO_INTERNAL, npipe_app_set_policy(&np, (const uint8_t *)"a", 1, 2, &changed));
    post("k", "b", 0, 1, 0, NULL); /* tracked in RAM even if it cannot be saved */
    TEST_ASSERT_NOT_NULL(npipe_app_find(&np, (const uint8_t *)"b", 1));
    apps_kv.power = NULL;
    npipe_apps_flush(&np); /* the dirty records are written now */
    boot_pipeline(SLOTS, APPS);
    TEST_ASSERT_NOT_NULL(npipe_app_find(&np, (const uint8_t *)"b", 1));
    TEST_ASSERT_EQUAL_UINT8(PROTO_APP_POLICY_BLOCK, npipe_app_find(&np, (const uint8_t *)"a", 1)->policy);
}

static void test_seen_apps_corrupt_records(void)
{
    const kv_storage_t *kv = &apps_kv.iface;
    uint8_t good[] = {'A', 1, 1, 2, 5, 0, 0, 0, 9, 0, 0, 0, 1, 'g', 0};
    kv->set(kv->ctx, "app000", good, sizeof(good));
    kv->set(kv->ctx, "app001", good, sizeof(good));                    /* duplicate id */
    kv->set(kv->ctx, "app002", "junk", 4);                             /* too short */
    uint8_t bad_len[] = {'A', 1, 1, 2, 5, 0, 0, 0, 9, 0, 0, 0, 1, 'x', 5};
    kv->set(kv->ctx, "app003", bad_len, sizeof(bad_len));              /* name past the end */
    uint8_t odd[] = {'A', 1, 9, 9, 5, 0, 0, 0, 9, 0, 0, 0, 1, 'o', 1, 'N'};
    kv->set(kv->ctx, "app004", odd, sizeof(odd)); /* outside n_apps: never read */
    boot_pipeline(SLOTS, APPS);
    TEST_ASSERT_EQUAL_UINT(1, npipe_app_count(&np));
    TEST_ASSERT_EQUAL_UINT(2, kv_mem_count(&apps_kv)); /* bad records in range erased */
    kv->set(kv->ctx, "app001", odd, sizeof(odd));
    uint8_t bad_ver[sizeof(good)];
    memcpy(bad_ver, good, sizeof(good));
    bad_ver[1] = 2;
    kv->set(kv->ctx, "app002", bad_ver, sizeof(bad_ver));
    boot_pipeline(SLOTS, APPS);
    const npipe_app_t *o = npipe_app_find(&np, (const uint8_t *)"o", 1);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT8(PROTO_APP_POLICY_DEFAULT, o->policy); /* unknown values fall back */
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_UNKNOWN, o->source);
    TEST_ASSERT_EQUAL_STRING("N", o->name);
    TEST_ASSERT_NOT_NULL(npipe_app_find(&np, (const uint8_t *)"g", 1));
    TEST_ASSERT_EQUAL_UINT(2, npipe_app_count(&np));
}

/* ---------------------------------------------------------------- messages */

static const char *handle(const char *req_json)
{
    static char out[4096];
    uint8_t buf[2048], rsp[PROTO_LIMIT_MAX_MESSAGE_CONFIG + 64];
    proto_msg_t m = parse_json_msg(req_json, buf, sizeof(buf));
    proto_writer_t w;
    proto_writer_init(&w, rsp, sizeof(rsp));
    TEST_ASSERT_TRUE(npipe_handle(&np, &m, &w));
    TEST_ASSERT_TRUE(proto_writer_ok(&w));
    TEST_ASSERT_LESS_OR_EQUAL(PROTO_LIMIT_MAX_MESSAGE_CONFIG, w.len);
    char *s = msg_to_json_str(rsp, w.len);
    snprintf(out, sizeof(out), "%s", s);
    free(s);
    return out;
}

static void test_msg_notifications(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"NOTIF_POST_RSP\",\"req_id\":30,\"fields\":{\"status\":0,\"notif_id\":1,\"shown\":true}}",
        handle("{\"type\":\"NOTIF_POST\",\"req_id\":30,\"fields\":{\"key\":\"k1\",\"app_id\":\"com.zing.zalo\","
               "\"app_name\":\"Zalo\",\"title\":\"Hi\",\"body\":\"B\",\"timestamp\":5,\"category\":4,"
               "\"priority\":2,\"flags\":2}}"));
    const npipe_notif_t *n = npipe_get(&np, 1);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_HIGH, n->priority);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CATEGORY_SOCIAL, n->category);
    TEST_ASSERT_EQUAL_UINT32(5, n->timestamp);
    set_cfg(PROTO_CFG_NOTIF_MIN_PRIORITY, PROTO_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"NOTIF_POST_RSP\",\"req_id\":31,\"fields\":{\"status\":0,\"notif_id\":2,\"shown\":false}}",
        handle("{\"type\":\"NOTIF_POST\",\"req_id\":31,\"fields\":{\"key\":\"k\",\"app_id\":\"a\"}}"));
    TEST_ASSERT_EQUAL_UINT8(PROTO_PRIORITY_NORMAL, npipe_get(&np, 2)->priority); /* absent -> normal */
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"NOTIF_REMOVE_RSP\",\"req_id\":34,\"fields\":{\"status\":24}}",
                             handle("{\"type\":\"NOTIF_REMOVE\",\"req_id\":34,\"fields\":{\"key\":\"zz\"}}"));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"NOTIF_REMOVE_RSP\",\"req_id\":35,\"fields\":{\"status\":0}}",
                             handle("{\"type\":\"NOTIF_REMOVE\",\"req_id\":35,\"fields\":{\"key\":\"k\"}}"));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"NOTIF_CLEAR_RSP\",\"req_id\":36,\"fields\":{\"status\":0}}",
                             handle("{\"type\":\"NOTIF_CLEAR\",\"req_id\":36}"));
    TEST_ASSERT_EQUAL_UINT(0, npipe_count(&np, false));

    uint8_t buf[16], rsp[16];
    proto_msg_t m = parse_json_msg("{\"type\":\"CONFIG_RESET\"}", buf, sizeof(buf));
    proto_writer_t w;
    proto_writer_init(&w, rsp, sizeof(rsp));
    TEST_ASSERT_FALSE(npipe_handle(&np, &m, &w));
}

static void test_msg_apps(void)
{
    clock_now = 0;
    post("k", "com.a", 0, 1, 0, NULL);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_SET_POLICY_RSP\",\"req_id\":21,\"fields\":{\"status\":0,\"revision\":1}}",
        handle("{\"type\":\"APPS_SET_POLICY\",\"req_id\":21,\"fields\":{\"entry\":["
               "{\"app_id\":\"com.facebook.orca\",\"policy\":2},{\"app_id\":\"com.a\",\"policy\":1}]}}"));
    TEST_ASSERT_EQUAL_INT(1, n_cfg_events);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_LIST_RSP\",\"req_id\":20,\"fields\":{\"status\":0,\"total\":2,\"app\":["
        "{\"app_id\":\"com.a\",\"policy\":1,\"source\":2},{\"app_id\":\"com.facebook.orca\",\"policy\":2,\"source\":0}]}}",
        handle("{\"type\":\"APPS_LIST\",\"req_id\":20}"));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_LIST_RSP\",\"req_id\":22,\"fields\":{\"status\":0,\"total\":2,\"app\":["
        "{\"app_id\":\"com.facebook.orca\",\"policy\":2,\"source\":0}]}}",
        handle("{\"type\":\"APPS_LIST\",\"req_id\":22,\"fields\":{\"offset\":1,\"limit\":5}}"));
    /* same policies again: nothing changes, revision kept */
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_SET_POLICY_RSP\",\"req_id\":23,\"fields\":{\"status\":0,\"revision\":1}}",
        handle("{\"type\":\"APPS_SET_POLICY\",\"req_id\":23,\"fields\":{\"entry\":["
               "{\"app_id\":\"com.a\",\"policy\":1}]}}"));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_FORGET_RSP\",\"req_id\":24,\"fields\":{\"status\":0,\"revision\":2}}",
        handle("{\"type\":\"APPS_FORGET\",\"req_id\":24,\"fields\":{\"app_id\":[\"com.a\",\"nope\"]}}"));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_FORGET_RSP\",\"req_id\":25,\"fields\":{\"status\":0,\"revision\":2}}",
        handle("{\"type\":\"APPS_FORGET\",\"req_id\":25,\"fields\":{\"app_id\":[\"nope\"]}}"));
    /* fill with explicit policies, then one more: NO_SPACE, earlier entries stay applied */
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_SET_POLICY_RSP\",\"req_id\":26,\"fields\":{\"status\":15}}",
        handle("{\"type\":\"APPS_SET_POLICY\",\"req_id\":26,\"fields\":{\"entry\":["
               "{\"app_id\":\"b\",\"policy\":1},{\"app_id\":\"c\",\"policy\":1},{\"app_id\":\"d\",\"policy\":1},"
               "{\"app_id\":\"e\",\"policy\":1},{\"app_id\":\"f\",\"policy\":1}]}}"));
    TEST_ASSERT_EQUAL_UINT32(3, cfg_revision(&cfg));
    TEST_ASSERT_EQUAL_UINT(APPS, npipe_app_count(&np));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_FORGET_RSP\",\"req_id\":27,\"fields\":{\"status\":0,\"revision\":4}}",
        handle("{\"type\":\"APPS_FORGET\",\"req_id\":27}"));
    TEST_ASSERT_EQUAL_UINT(0, npipe_app_count(&np));
    /* revision bump failing in storage */
    power = 0;
    cfg_kv.power = &power;
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"APPS_SET_POLICY_RSP\",\"req_id\":28,\"fields\":{\"status\":27}}",
        handle("{\"type\":\"APPS_SET_POLICY\",\"req_id\":28,\"fields\":{\"entry\":[{\"app_id\":\"b\",\"policy\":1}]}}"));
    cfg_kv.power = NULL;
}

static void test_msg_apps_list_fits_channel(void)
{
    static npipe_app_t many[PROTO_LIMIT_SEEN_APPS_MAX];
    npipe_params_t p = {.cfg = &cfg, .kv = &apps_kv.iface, .slots = slots, .n_slots = SLOTS, .apps = many,
                        .n_apps = PROTO_LIMIT_SEEN_APPS_MAX, .now_utc = now_utc};
    npipe_init(&np, &p);
    char id[65], name[65];
    memset(name, 'n', 64);
    name[64] = '\0';
    for (int i = 0; i < 20; i++) {
        memset(id, 'a' + i, 64);
        id[64] = '\0';
        npipe_android_t a = {.key = S("k"), .app_id = S(id), .app_name = S(name)};
        uint32_t nid;
        bool sh;
        npipe_android_post(&np, &a, &nid, &sh);
    }
    const char *rsp = handle("{\"type\":\"APPS_LIST\",\"req_id\":1,\"fields\":{\"limit\":16}}");
    int entries = 0;
    for (const char *s = rsp; (s = strstr(s, "\"app_id\"")) != NULL; s++) {
        entries++;
    }
    TEST_ASSERT_EQUAL_INT(6, entries); /* 13 + 6 x 152 bytes fit in 1024, a 7th entry would not */
    TEST_ASSERT_NOT_NULL(strstr(rsp, "\"total\":20"));
}

static void test_app_seen_event_matches_vector(void)
{
    npipe_app_t a = {.used = true, .policy = 0, .source = PROTO_NOTIF_SOURCE_ANCS};
    strcpy(a.id, "com.apple.MobileSMS");
    a.id_len = (uint8_t)strlen(a.id);
    strcpy(a.name, "Tin nh\xe1\xba\xafn");
    a.name_len = (uint8_t)strlen(a.name);
    uint8_t out[256];
    proto_writer_t w;
    proto_writer_init(&w, out, sizeof(out));
    npipe_write_app_seen_event(&w, &a);
    char hex[513];
    hex_encode(out, w.len, hex);
    cJSON *doc = load_vectors("messages.json");
    const cJSON *c;
    bool found = false;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc, "cases")) {
        if (strcmp(jstr(c, "name"), "event_app_seen") == 0) {
            TEST_ASSERT_EQUAL_STRING(jstr(c, "hex"), hex);
            found = true;
        }
    }
    cJSON_Delete(doc);
    TEST_ASSERT_TRUE(found);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_android_lifecycle);
    RUN_TEST(test_dismiss);
    RUN_TEST(test_unknown_enums_fall_back);
    RUN_TEST(test_long_text_truncated_on_board);
    RUN_TEST(test_clear_only_android);
    RUN_TEST(test_filters);
    RUN_TEST(test_app_policies);
    RUN_TEST(test_quiet_hours);
    RUN_TEST(test_modify_transitions);
    RUN_TEST(test_store_eviction_and_listing);
    RUN_TEST(test_ancs_mapping);
    RUN_TEST(test_ancs_events_and_session_reset);
    RUN_TEST(test_ancs_app_id_sanitised);
    RUN_TEST(test_seen_apps_tracking_and_persistence);
    RUN_TEST(test_seen_apps_lru_keeps_explicit_policies);
    RUN_TEST(test_seen_apps_storage_failures);
    RUN_TEST(test_seen_apps_corrupt_records);
    RUN_TEST(test_msg_notifications);
    RUN_TEST(test_msg_apps);
    RUN_TEST(test_msg_apps_list_fits_channel);
    RUN_TEST(test_app_seen_event_matches_vector);
    return UNITY_END();
}
