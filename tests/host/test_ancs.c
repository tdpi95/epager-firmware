/* ANCS client state machine (components/ancs_client/src/ancs_sm.c): the Control Point requests it writes,
 * what it does with Data Source fragments, removals, errors and timeouts, against the real notification
 * pipeline. The glue (NimBLE) is replaced by a recording write callback. */
#include <stdlib.h>
#include <string.h>

#include "epager/ancs_sm.h"
#include "test_support.h"
#include "unity.h"
#include "xcore.h"

static xcore_t *x;
static ancs_sm_t sm; /* about 4.5 KB */
static struct {
    uint8_t cmd[ANCS_CMD_MAX];
    size_t len;
} wr[64];
static int n_wr;
static bool link_up;

static bool write_cp(void *ctx, const uint8_t *cmd, size_t len)
{
    (void)ctx;
    if (!link_up) {
        return false;
    }
    TEST_ASSERT_TRUE(n_wr < 64);
    memcpy(wr[n_wr].cmd, cmd, len);
    wr[n_wr++].len = len;
    return true;
}

void setUp(void)
{
    x = xcore_new();
    n_wr = 0;
    link_up = true;
    ancs_sm_init(&sm, &(ancs_sm_params_t){.np = &x->core.np, .write_cp = write_cp});
    ancs_sm_session_start(&sm);
}
void tearDown(void) { xcore_free(x); }

static uint32_t T; /* the test clock, ms */

static void ns(uint8_t event, uint8_t flags, uint8_t category, uint32_t uid)
{
    uint8_t p[8] = {event, flags, category, 1, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24)};
    ancs_sm_on_ns(&sm, T, p, sizeof(p));
}

/* GetNotificationAttributes answer: command 0, the UID, then (id, u16 length, value) tuples in request order. */
static size_t notif_rsp(uint8_t *o, uint32_t uid, const char *app, const char *title, const char *sub, const char *msg,
                        const char *date)
{
    size_t n = 0;
    o[n++] = 0;
    for (int i = 0; i < 4; i++) {
        o[n++] = (uint8_t)(uid >> (8 * i));
    }
    const struct { uint8_t id; const char *v; } a[] = {{0, app}, {1, title}, {2, sub}, {3, msg}, {5, date}};
    for (int i = 0; i < 5; i++) {
        const size_t l = strlen(a[i].v);
        o[n++] = a[i].id;
        o[n++] = (uint8_t)l;
        o[n++] = (uint8_t)(l >> 8);
        memcpy(o + n, a[i].v, l);
        n += l;
    }
    return n;
}

/* GetAppAttributes answer: command 1, the app identifier and a NUL, then the display name tuple. */
static size_t app_rsp(uint8_t *o, const char *app, const char *name)
{
    size_t n = 0;
    o[n++] = 1;
    memcpy(o + n, app, strlen(app) + 1);
    n += strlen(app) + 1;
    const size_t l = strlen(name);
    o[n++] = 0;
    o[n++] = (uint8_t)l;
    o[n++] = (uint8_t)(l >> 8);
    memcpy(o + n, name, l);
    return n + l;
}

/* Feed a response in fragments of `frag` bytes (an MTU's worth; ANCS fragments carry no markers). */
static void ds_feed(const uint8_t *r, size_t n, size_t frag)
{
    for (size_t off = 0; off < n; off += frag) {
        ancs_sm_on_ds(&sm, T, r + off, n - off < frag ? n - off : frag);
    }
}

static void answer_notif(uint32_t uid, const char *app, const char *title, const char *msg)
{
    uint8_t r[1600];
    ds_feed(r, notif_rsp(r, uid, app, title, "", msg, "20261014T091500"), 20);
}

static void answer_app(const char *app, const char *name)
{
    uint8_t r[400];
    ds_feed(r, app_rsp(r, app, name), 20);
}

static void ok(void) { ancs_sm_write_done(&sm, T, 0); }

static void test_fetch_and_show(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_NEGATIVE_ACTION, PROTO_CATEGORY_EMAIL, 0x01020304);
    TEST_ASSERT_EQUAL_INT(1, n_wr);
    /* GetNotificationAttributes: command 0, UID, AppIdentifier, Title (128), Subtitle (128), Message (1024), Date */
    const uint8_t want[] = {0, 4, 3, 2, 1, 0, 1, 0x80, 0, 2, 0x80, 0, 3, 0x00, 0x04, 5};
    TEST_ASSERT_EQUAL_UINT(sizeof(want), wr[0].len);
    TEST_ASSERT_EQUAL_MEMORY(want, wr[0].cmd, sizeof(want));
    TEST_ASSERT_TRUE(ancs_sm_busy(&sm));
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);

    ok();
    answer_notif(0x01020304, "com.apple.mobilemail", "Hello", "Body text");
    /* an app we have not seen: its display name is asked for (command 1, identifier, NUL, attribute 0) */
    TEST_ASSERT_EQUAL_INT(2, n_wr);
    TEST_ASSERT_EQUAL_UINT8(1, wr[1].cmd[0]);
    TEST_ASSERT_EQUAL_MEMORY("com.apple.mobilemail", &wr[1].cmd[1], 21);
    TEST_ASSERT_EQUAL_UINT8(0, wr[1].cmd[21]);
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
    ok();
    answer_app("com.apple.mobilemail", "Mail");

    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    TEST_ASSERT_EQUAL_STRING("Hello", x->last_notif.title);
    TEST_ASSERT_EQUAL_STRING("Body text", x->last_notif.body);
    TEST_ASSERT_EQUAL_STRING("Mail", x->last_notif.app_name);
    TEST_ASSERT_EQUAL_STRING("com.apple.mobilemail", x->last_notif.app_id);
    TEST_ASSERT_EQUAL_UINT8(PROTO_NOTIF_SOURCE_ANCS, x->last_notif.source);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CATEGORY_EMAIL, x->last_notif.category);
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));

    /* the same app again: the name comes from the session cache, one request only */
    ns(PROTO_ANCS_EVENT_ADDED, 0, PROTO_CATEGORY_EMAIL, 9);
    ok();
    answer_notif(9, "com.apple.mobilemail", "Second", "");
    TEST_ASSERT_EQUAL_INT(3, n_wr);
    TEST_ASSERT_EQUAL_INT(2, x->n_notif_events);
    TEST_ASSERT_EQUAL_STRING("Mail", x->last_notif.app_name);
    TEST_ASSERT_EQUAL_UINT32(1, sm.n_shown - 1);
}

static void test_one_request_at_a_time(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 3);
    TEST_ASSERT_EQUAL_INT(1, n_wr); /* the others wait */
    ok();
    TEST_ASSERT_EQUAL_INT(1, n_wr); /* the answer is still to come */
    answer_notif(1, "a.b", "one", "");
    TEST_ASSERT_EQUAL_INT(2, n_wr); /* the app name of the first... */
    TEST_ASSERT_EQUAL_UINT8(1, wr[1].cmd[0]);
    ok();
    answer_app("a.b", "App");
    TEST_ASSERT_EQUAL_INT(3, n_wr); /* ...and only then the second notification */
    TEST_ASSERT_EQUAL_UINT8(0, wr[2].cmd[0]);
    TEST_ASSERT_EQUAL_UINT8(2, wr[2].cmd[1]);
}

static void test_data_source_before_write_response(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 5);
    answer_notif(5, "x.y", "early", ""); /* the answer overtakes the write response */
    TEST_ASSERT_EQUAL_INT(1, n_wr);      /* nothing more is sent until the write is settled */
    ok();
    TEST_ASSERT_EQUAL_INT(2, n_wr);      /* now the app name */
    ok();
    answer_app("x.y", "Why");
    TEST_ASSERT_EQUAL_STRING("early", x->last_notif.title);
}

static void test_removal_cancels_a_fetch(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 7);
    ok();
    ns(PROTO_ANCS_EVENT_REMOVED, 0, 0, 7);
    answer_notif(7, "a.b", "gone", "");
    TEST_ASSERT_EQUAL_INT(1, n_wr);          /* no app name request for a cancelled one */
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
    TEST_ASSERT_EQUAL_UINT32(1, sm.n_dropped);
}

static void test_removal_of_a_queued_notification_and_of_a_shown_one(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ns(PROTO_ANCS_EVENT_REMOVED, 0, 0, 2); /* still waiting: never fetched */
    ok();
    answer_notif(1, "a.b", "kept", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_INT(2, n_wr);
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    const uint32_t id = x->last_notif.id;
    ns(PROTO_ANCS_EVENT_REMOVED, 0, 0, 1); /* a shown one: removed at once, no request */
    TEST_ASSERT_EQUAL_INT(2, x->n_notif_events);
    TEST_ASSERT_EQUAL_UINT8(NPIPE_REMOVED, x->last_notif.kind);
    TEST_ASSERT_EQUAL_UINT32(id, x->last_notif.id);
    TEST_ASSERT_EQUAL_INT(2, n_wr);
}

static void test_modified_for_an_unknown_notification_is_fetched_like_an_add(void)
{
    ns(PROTO_ANCS_EVENT_MODIFIED, 0, 0, 33);
    ok();
    answer_notif(33, "a.b", "changed", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_STRING("changed", x->last_notif.title);
}

static void test_ancs_errors_drop_only_that_notification(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ancs_sm_write_done(&sm, T, 0xA2); /* invalid parameter: the notification is already gone */
    TEST_ASSERT_EQUAL_INT(2, n_wr);   /* the next one goes out */
    TEST_ASSERT_EQUAL_UINT8(2, wr[1].cmd[1]);
    ok();
    answer_notif(2, "a.b", "second", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    TEST_ASSERT_EQUAL_STRING("second", x->last_notif.title);
}

static void test_app_name_failure_shows_the_notification_without_a_name(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ok();
    answer_notif(1, "no.name", "title", "");
    ancs_sm_write_done(&sm, T, 0xA2); /* iOS has no such app */
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    TEST_ASSERT_EQUAL_STRING("title", x->last_notif.title);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2); /* not asked about again this session */
    ok();
    answer_notif(2, "no.name", "again", "");
    TEST_ASSERT_EQUAL_INT(3, n_wr);
    TEST_ASSERT_EQUAL_INT(2, x->n_notif_events);
}

static void test_timeout_drops_the_request_and_moves_on(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ok();
    uint8_t r[64];
    const size_t n = notif_rsp(r, 1, "a.b", "t", "", "", "");
    ancs_sm_on_ds(&sm, T, r, n / 2); /* half an answer, then silence */
    T += ANCS_TIMEOUT_MS - 1;
    ancs_sm_poll(&sm, T);
    TEST_ASSERT_EQUAL_INT(1, n_wr);
    T += 1;
    ancs_sm_poll(&sm, T);
    TEST_ASSERT_EQUAL_INT(2, n_wr); /* the second notification's request */
    TEST_ASSERT_EQUAL_UINT8(2, wr[1].cmd[1]);
    TEST_ASSERT_EQUAL_UINT32(1, sm.n_timeouts);
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
    /* a write whose response never came does not block the session either */
    T += ANCS_TIMEOUT_MS;
    ancs_sm_poll(&sm, T);
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
}

static void test_a_late_write_result_is_not_the_next_ones(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    T += ANCS_TIMEOUT_MS;
    ancs_sm_poll(&sm, T); /* uid 1: its write never answered */
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(2, n_wr);
    ok(); /* the answer to the first write finally arrives */
    answer_notif(2, "a.b", "second", "");
    TEST_ASSERT_EQUAL_INT(2, n_wr); /* the second write is still unanswered: no app name request yet */
    ok();
    TEST_ASSERT_EQUAL_INT(3, n_wr);
}

static void test_a_stray_first_fragment_is_discarded(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ok();
    uint8_t r[400];
    ds_feed(r, notif_rsp(r, 999, "other", "stray", "", "", ""), 20); /* the answer to an old request */
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
    TEST_ASSERT_TRUE(ancs_sm_busy(&sm)); /* still waiting for ours */
    answer_notif(1, "a.b", "mine", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_STRING("mine", x->last_notif.title);
}

static void test_oversized_answer_drops_the_notification(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ok();
    static char big[1300];
    memset(big, 'a', sizeof(big) - 1);
    static uint8_t r[3000];
    /* more than the buffer holds: a title and a message of 1,299 bytes each (iOS honours our limits; a bad one might not) */
    ds_feed(r, notif_rsp(r, 1, "a.b", big, "", big, ""), 100);
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
    TEST_ASSERT_EQUAL_UINT32(1, sm.n_dropped);
    TEST_ASSERT_EQUAL_INT(2, n_wr);       /* the session goes on with the next one */
    TEST_ASSERT_EQUAL_UINT8(2, wr[1].cmd[1]);
}

static void test_queue_overflow_keeps_the_newest(void)
{
    for (uint32_t uid = 1; uid <= ANCS_QUEUE_MAX + 10; uid++) {
        ns(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_PRE_EXISTING, 0, uid);
    }
    TEST_ASSERT_EQUAL_UINT32(9, sm.n_dropped); /* 41 waiting for a queue of 32 */
    TEST_ASSERT_EQUAL_UINT8(1, wr[0].cmd[1]); /* uid 1 was already in flight when the queue filled */
    ok();
    answer_notif(1, "a.b", "t", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_UINT8(11, wr[2].cmd[1]); /* uids 2-10 were dropped: the oldest waiting ones */
}

static void test_same_uid_updates_the_queue_entry(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2);
    ns(PROTO_ANCS_EVENT_MODIFIED, 0, 0, 2);
    TEST_ASSERT_EQUAL_UINT32(0, sm.n_dropped);
    TEST_ASSERT_EQUAL_UINT(1, sm.n_queue);
}

static void test_negative_action(void)
{
    TEST_ASSERT_TRUE(ancs_sm_negative_action(&sm, 0x11223344));
    ancs_sm_poll(&sm, T);
    TEST_ASSERT_EQUAL_INT(1, n_wr);
    const uint8_t want[] = {2, 0x44, 0x33, 0x22, 0x11, 1}; /* PerformNotificationAction, UID, negative */
    TEST_ASSERT_EQUAL_UINT(sizeof(want), wr[0].len);
    TEST_ASSERT_EQUAL_MEMORY(want, wr[0].cmd, sizeof(want));

    /* serialised with the fetches: nothing else is written until the result is in */
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    TEST_ASSERT_EQUAL_INT(1, n_wr);
    ok();
    TEST_ASSERT_EQUAL_INT(2, n_wr);
    TEST_ASSERT_EQUAL_UINT8(0, wr[1].cmd[0]);

    /* an action while a fetch's answer is awaited goes out once its write is settled */
    ok();
    TEST_ASSERT_TRUE(ancs_sm_negative_action(&sm, 5));
    ancs_sm_poll(&sm, T);
    TEST_ASSERT_EQUAL_INT(3, n_wr);
    TEST_ASSERT_EQUAL_UINT8(2, wr[2].cmd[0]);
    ok(); /* an action's failure (0xA3) changes nothing else */
    ancs_sm_write_done(&sm, T, 0xA3);
    for (unsigned i = 0; i < ANCS_ACTION_MAX; i++) {
        TEST_ASSERT_TRUE(ancs_sm_negative_action(&sm, 100u + i));
    }
    TEST_ASSERT_FALSE(ancs_sm_negative_action(&sm, 200)); /* the queue is bounded */
}

static void test_session_start_drops_ancs_notifications_and_the_name_cache(void)
{
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    ok();
    answer_notif(1, "a.b", "old", "");
    ok();
    answer_app("a.b", "A");
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events);
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 2); /* mid-fetch when the link drops */
    ancs_sm_session_end(&sm);
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
    TEST_ASSERT_EQUAL_INT(1, x->n_notif_events); /* the shown one stays while the phone is away */

    ancs_sm_session_start(&sm); /* iOS replays what is still there */
    TEST_ASSERT_EQUAL_INT(2, x->n_notif_events);
    TEST_ASSERT_EQUAL_UINT8(NPIPE_REMOVED, x->last_notif.kind);
    n_wr = 0;
    ns(PROTO_ANCS_EVENT_ADDED, PROTO_ANCS_FLAG_PRE_EXISTING, 0, 1);
    ok();
    answer_notif(1, "a.b", "old", "");
    TEST_ASSERT_EQUAL_INT(2, n_wr); /* the name is asked for again: a new session */
}

static void test_no_link_drops_the_request(void)
{
    link_up = false;
    ns(PROTO_ANCS_EVENT_ADDED, 0, 0, 1);
    TEST_ASSERT_EQUAL_INT(0, n_wr);
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
    TEST_ASSERT_EQUAL_UINT32(1, sm.n_dropped);
}

static void test_garbage_is_ignored(void)
{
    const uint8_t shortp[3] = {0, 0, 0}, unknown_event[8] = {9, 0, 0, 1, 1, 0, 0, 0};
    ancs_sm_on_ns(&sm, T, shortp, sizeof(shortp));
    ancs_sm_on_ns(&sm, T, unknown_event, sizeof(unknown_event));
    ancs_sm_on_ns(&sm, T, NULL, 0);
    uint8_t r[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    ancs_sm_on_ds(&sm, T, r, sizeof(r)); /* no request outstanding */
    ancs_sm_write_done(&sm, T, 0);       /* no write outstanding */
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
    TEST_ASSERT_EQUAL_INT(0, n_wr);
    TEST_ASSERT_EQUAL_INT(0, x->n_notif_events);
}

/* Random inputs of every kind in any order: nothing crashes, and the machine always settles (ASan/UBSan). */
static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static void test_fuzz(void)
{
#define RND() rnd()
    uint8_t buf[300];
    for (int i = 0; i < 100000; i++) {
        T += RND() % 700;
        switch (RND() % 6) {
        case 0:
            { const uint8_t ev = (uint8_t)(RND() % 4), fl = (uint8_t)RND(), cat = (uint8_t)(RND() % 14); ns(ev, fl, cat, RND() % 40); }
            break;
        case 1: {
            const size_t n = notif_rsp(buf, RND() % 40, "app.id", "title", "sub", "message body", "20261014T091500");
            const size_t cut = RND() % (n + 1);
            ancs_sm_on_ds(&sm, T, buf, cut);
            ancs_sm_on_ds(&sm, T, buf + cut, n - cut);
            break;
        }
        case 2: {
            const size_t n = app_rsp(buf, "app.id", "App");
            ancs_sm_on_ds(&sm, T, buf, n);
            break;
        }
        case 3:
            for (size_t k = 0; k < sizeof(buf); k++) {
                buf[k] = (uint8_t)RND();
            }
            ancs_sm_on_ds(&sm, T, buf, RND() % sizeof(buf));
            break;
        case 4:
            ancs_sm_write_done(&sm, T, RND() % 3 == 0 ? 0xA2 : 0);
            break;
        default:
            ancs_sm_negative_action(&sm, RND() % 40);
            ancs_sm_poll(&sm, T);
            break;
        }
        n_wr = 0;
        if (RND() % 5000 == 0) {
            ancs_sm_session_start(&sm);
        }
    }
    T += 10 * ANCS_TIMEOUT_MS; /* silence: every request times out, the machine goes idle */
    for (int i = 0; i < 100; i++) {
        ancs_sm_poll(&sm, T);
        n_wr = 0;
        if (ancs_sm_busy(&sm)) {
            ancs_sm_write_done(&sm, T, 0xA2);
            T += ANCS_TIMEOUT_MS;
        }
    }
    TEST_ASSERT_FALSE(ancs_sm_busy(&sm));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fetch_and_show);
    RUN_TEST(test_one_request_at_a_time);
    RUN_TEST(test_data_source_before_write_response);
    RUN_TEST(test_removal_cancels_a_fetch);
    RUN_TEST(test_removal_of_a_queued_notification_and_of_a_shown_one);
    RUN_TEST(test_modified_for_an_unknown_notification_is_fetched_like_an_add);
    RUN_TEST(test_ancs_errors_drop_only_that_notification);
    RUN_TEST(test_app_name_failure_shows_the_notification_without_a_name);
    RUN_TEST(test_timeout_drops_the_request_and_moves_on);
    RUN_TEST(test_a_late_write_result_is_not_the_next_ones);
    RUN_TEST(test_a_stray_first_fragment_is_discarded);
    RUN_TEST(test_oversized_answer_drops_the_notification);
    RUN_TEST(test_queue_overflow_keeps_the_newest);
    RUN_TEST(test_same_uid_updates_the_queue_entry);
    RUN_TEST(test_negative_action);
    RUN_TEST(test_session_start_drops_ancs_notifications_and_the_name_cache);
    RUN_TEST(test_no_link_drops_the_request);
    RUN_TEST(test_garbage_is_ignored);
    RUN_TEST(test_fuzz);
    return UNITY_END();
}
