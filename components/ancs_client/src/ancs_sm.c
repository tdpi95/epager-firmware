#include "epager/ancs_sm.h"

#include <string.h>

#include "epager/proto_gen.h"

/* What is asked for (protocol.md 6.2): the app, the text with the limits of the NotificationEvent, the date. */
static const proto_ancs_attr_req_t NOTIF_REQ[] = {
    {PROTO_ANCS_ATTR_APP_ID, 0},
    {PROTO_ANCS_ATTR_TITLE, PROTO_LIMIT_ANCS_TITLE_MAX},
    {PROTO_ANCS_ATTR_SUBTITLE, PROTO_LIMIT_ANCS_SUBTITLE_MAX},
    {PROTO_ANCS_ATTR_MESSAGE, PROTO_LIMIT_ANCS_MESSAGE_MAX},
    {PROTO_ANCS_ATTR_DATE, 0},
};
#define N_NOTIF_REQ (sizeof(NOTIF_REQ) / sizeof(NOTIF_REQ[0]))
static const uint8_t APP_ATTR[] = {PROTO_ANCS_APP_ATTR_DISPLAY_NAME};

enum { WRITE_NONE = 0, WRITE_FETCH, WRITE_ACTION }; /* ancs_sm_t.write_kind: what the pending write was */

static bool expired(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void reset(ancs_sm_t *s)
{
    const ancs_sm_params_t p = s->p;
    memset(s, 0, sizeof(*s));
    s->p = p;
}

void ancs_sm_init(ancs_sm_t *s, const ancs_sm_params_t *p)
{
    memset(s, 0, sizeof(*s));
    s->p = *p;
}

void ancs_sm_session_start(ancs_sm_t *s)
{
    reset(s);
    npipe_ancs_session_reset(s->p.np);
}

void ancs_sm_session_end(ancs_sm_t *s)
{
    reset(s);
}

bool ancs_sm_busy(const ancs_sm_t *s)
{
    return s->phase != ANCS_PHASE_NONE || s->n_queue > 0 || s->n_actions > 0 || s->write_pending;
}

/* ---------------------------------------------------------------- app name cache */

static const ancs_app_t *cache_find(const ancs_sm_t *s, const uint8_t *id, size_t len)
{
    for (size_t i = 0; i < ANCS_APP_CACHE_MAX; i++) {
        if (s->apps[i].used && s->apps[i].id_len == len && memcmp(s->apps[i].id, id, len) == 0) {
            return &s->apps[i];
        }
    }
    return NULL;
}

static void cache_put(ancs_sm_t *s, const uint8_t *id, size_t id_len, const uint8_t *name, size_t name_len)
{
    if (id_len == 0 || id_len > ANCS_APP_ID_MAX || cache_find(s, id, id_len) != NULL) {
        return;
    }
    size_t at = s->next_evict;
    for (size_t i = 0; i < ANCS_APP_CACHE_MAX; i++) {
        if (!s->apps[i].used) {
            at = i;
            break;
        }
    }
    if (s->apps[at].used) {
        s->next_evict = (s->next_evict + 1) % ANCS_APP_CACHE_MAX;
    }
    ancs_app_t *a = &s->apps[at];
    a->used = true;
    a->id_len = (uint8_t)id_len;
    memcpy(a->id, id, id_len);
    a->name_len = (uint8_t)(name_len < ANCS_APP_NAME_MAX ? name_len : ANCS_APP_NAME_MAX); /* cut: npipe sanitises */
    if (a->name_len > 0) {
        memcpy(a->name, name, a->name_len);
    }
}

/* ---------------------------------------------------------------- the request in flight */

static bool send_cp(ancs_sm_t *s, uint8_t kind, const uint8_t *cmd, size_t len)
{
    if (len == 0) {
        return false;
    }
    s->write_pending = true;
    s->write_kind = kind;
    if (!s->p.write_cp(s->p.ctx, cmd, len)) {
        s->write_pending = false;
        s->write_kind = WRITE_NONE;
        return false;
    }
    return true;
}

static void drop_current(ancs_sm_t *s)
{
    s->phase = ANCS_PHASE_NONE;
    s->n_dropped++;
}

static void emit(ancs_sm_t *s, const uint8_t *name, size_t name_len)
{
    s->phase = ANCS_PHASE_NONE;
    if (s->cancelled) {
        s->n_dropped++;
        return;
    }
    npipe_ancs_t in;
    npipe_ancs_from_ds(&in, &s->cur, &s->ds_notif, (npipe_str_t){name, name_len});
    npipe_ancs_event(s->p.np, &in);
    s->n_shown++;
}

/* The request failed (error, timeout, junk): a notification without its attributes is dropped; one that
 * only lacks the app's display name is shown without it (and the app is not asked about again). */
static void fail_current(ancs_sm_t *s)
{
    s->n_errors++;
    if (s->phase == ANCS_PHASE_APP) {
        cache_put(s, s->app_id, s->app_id_len, NULL, 0);
        emit(s, NULL, 0);
    } else if (s->phase == ANCS_PHASE_NOTIF) {
        drop_current(s);
    }
}

static void request_app_name(ancs_sm_t *s, uint32_t now_ms, const uint8_t *id, size_t len)
{
    uint8_t cmd[ANCS_CMD_MAX];
    memcpy(s->app_id, id, len);
    s->app_id_len = len;
    proto_ancs_ds_init_app(&s->ds_app, s->app_id, len, APP_ATTR, sizeof(APP_ATTR), s->buf_app, sizeof(s->buf_app));
    s->phase = ANCS_PHASE_APP;
    s->ds_complete = false;
    s->deadline_ms = now_ms + ANCS_TIMEOUT_MS;
    if (!send_cp(s, WRITE_FETCH, cmd, proto_ancs_build_get_app_attrs(id, len, APP_ATTR, sizeof(APP_ATTR), cmd, sizeof(cmd)))) {
        fail_current(s);
    }
}

/* The write was accepted and the whole Data Source answer is in. */
static void maybe_finish(ancs_sm_t *s, uint32_t now_ms)
{
    if (s->phase == ANCS_PHASE_NONE || !s->ds_complete || (s->write_pending && s->write_kind == WRITE_FETCH)) {
        return;
    }
    if (s->phase == ANCS_PHASE_NOTIF) {
        if (s->cancelled) {
            drop_current(s);
            return;
        }
        const uint8_t *id;
        uint16_t id_len;
        if (!proto_ancs_ds_attr(&s->ds_notif, PROTO_ANCS_ATTR_APP_ID, &id, &id_len) || id_len == 0 ||
            id_len > ANCS_APP_ID_MAX) {
            emit(s, NULL, 0);
            return;
        }
        const ancs_app_t *a = cache_find(s, id, id_len);
        if (a != NULL) {
            emit(s, a->name, a->name_len);
        } else {
            request_app_name(s, now_ms, id, id_len);
        }
        return;
    }
    const uint8_t *name = NULL;
    uint16_t name_len = 0;
    if (!proto_ancs_ds_attr(&s->ds_app, PROTO_ANCS_APP_ATTR_DISPLAY_NAME, &name, &name_len)) {
        name_len = 0;
    }
    cache_put(s, s->app_id, s->app_id_len, name, name_len);
    const ancs_app_t *a = cache_find(s, s->app_id, s->app_id_len);
    emit(s, a != NULL ? a->name : NULL, a != NULL ? a->name_len : 0);
}

static void start_fetch(ancs_sm_t *s, uint32_t now_ms)
{
    uint8_t cmd[ANCS_CMD_MAX], ids[N_NOTIF_REQ];
    s->cur = s->queue[0];
    memmove(&s->queue[0], &s->queue[1], (s->n_queue - 1) * sizeof(s->queue[0]));
    s->n_queue--;
    for (size_t i = 0; i < N_NOTIF_REQ; i++) {
        ids[i] = NOTIF_REQ[i].id;
    }
    s->cancelled = false;
    s->ds_complete = false;
    s->phase = ANCS_PHASE_NOTIF;
    s->deadline_ms = now_ms + ANCS_TIMEOUT_MS;
    proto_ancs_ds_init_notif(&s->ds_notif, s->cur.uid, ids, N_NOTIF_REQ, s->buf_notif, sizeof(s->buf_notif));
    if (!send_cp(s, WRITE_FETCH, cmd,
                 proto_ancs_build_get_notif_attrs(s->cur.uid, NOTIF_REQ, N_NOTIF_REQ, cmd, sizeof(cmd)))) {
        fail_current(s);
    }
}

/* ---------------------------------------------------------------- inputs */

static void dequeue_uid(ancs_sm_t *s, uint32_t uid)
{
    for (size_t i = 0; i < s->n_queue; i++) {
        if (s->queue[i].uid == uid) {
            memmove(&s->queue[i], &s->queue[i + 1], (s->n_queue - i - 1) * sizeof(s->queue[0]));
            s->n_queue--;
            return;
        }
    }
}

void ancs_sm_on_ns(ancs_sm_t *s, uint32_t now_ms, const uint8_t *v, size_t len)
{
    proto_ancs_ns_t ns;
    if (proto_ancs_parse_ns(v, len, &ns) != PROTO_OK || ns.event_id > PROTO_ANCS_EVENT_REMOVED) {
        s->n_errors++; /* garbage, or an event from a newer iOS: ignored */
        return;
    }
    if (ns.event_id == PROTO_ANCS_EVENT_REMOVED) {
        dequeue_uid(s, ns.uid);
        if (s->phase != ANCS_PHASE_NONE && s->cur.uid == ns.uid) {
            s->cancelled = true;
        }
        npipe_ancs_t in = {.ns = ns}; /* removal needs no attributes */
        npipe_ancs_event(s->p.np, &in);
    } else {
        for (size_t i = 0; i < s->n_queue; i++) {
            if (s->queue[i].uid == ns.uid) {
                s->queue[i] = ns; /* the newest event for a UID wins */
                ancs_sm_poll(s, now_ms);
                return;
            }
        }
        if (s->n_queue == ANCS_QUEUE_MAX) { /* the newest ones matter most */
            memmove(&s->queue[0], &s->queue[1], (ANCS_QUEUE_MAX - 1) * sizeof(s->queue[0]));
            s->n_queue--;
            s->n_dropped++;
        }
        s->queue[s->n_queue++] = ns;
    }
    ancs_sm_poll(s, now_ms);
}

void ancs_sm_on_ds(ancs_sm_t *s, uint32_t now_ms, const uint8_t *v, size_t len)
{
    if (s->phase == ANCS_PHASE_NONE) {
        return; /* an answer to a request that was dropped */
    }
    proto_ancs_ds_t *ds = s->phase == ANCS_PHASE_NOTIF ? &s->ds_notif : &s->ds_app;
    if (ds->done) {
        return;
    }
    const size_t before = ds->len;
    bool complete = false;
    const proto_status_t st = proto_ancs_ds_feed(ds, v, len, &complete);
    if (st != PROTO_OK) {
        if (before == 0 && st == PROTO_MALFORMED) {
            ds->len = 0; /* a first fragment that does not answer our request: not ours, discard it */
            s->n_errors++;
        } else {
            fail_current(s);
        }
    } else if (complete) {
        s->ds_complete = true;
        maybe_finish(s, now_ms);
    }
    ancs_sm_poll(s, now_ms);
}

void ancs_sm_write_done(ancs_sm_t *s, uint32_t now_ms, uint8_t att_status)
{
    if (s->stale_writes > 0) {
        s->stale_writes--; /* the late result of a write that timed out */
        return;
    }
    const uint8_t kind = s->write_kind;
    s->write_pending = false;
    s->write_kind = WRITE_NONE;
    if (att_status != 0) {
        s->n_errors++; /* ANCS 0xA0-0xA3, or the link failed */
        if (kind == WRITE_FETCH) {
            fail_current(s);
        }
    } else if (kind == WRITE_FETCH) {
        maybe_finish(s, now_ms);
    }
    ancs_sm_poll(s, now_ms);
}

bool ancs_sm_negative_action(ancs_sm_t *s, uint32_t uid)
{
    if (s->n_actions == ANCS_ACTION_MAX) {
        return false;
    }
    s->actions[s->n_actions].uid = uid;
    s->actions[s->n_actions].action = PROTO_ANCS_ACTION_NEGATIVE;
    s->n_actions++;
    return true;
}

void ancs_sm_poll(ancs_sm_t *s, uint32_t now_ms)
{
    if (s->phase != ANCS_PHASE_NONE && expired(now_ms, s->deadline_ms)) {
        s->n_timeouts++;
        if (s->write_pending && s->stale_writes < UINT8_MAX) {
            s->stale_writes++; /* ATT answers every request, late or never: that answer is not the next one's */
        }
        s->write_pending = false; /* a write that never answered must not block the session */
        s->write_kind = WRITE_NONE;
        fail_current(s);
    }
    if (s->write_pending) {
        return;
    }
    if (s->n_actions > 0) {
        uint8_t cmd[ANCS_CMD_MAX];
        const uint32_t uid = s->actions[0].uid;
        const uint8_t action = s->actions[0].action;
        memmove(&s->actions[0], &s->actions[1], (s->n_actions - 1) * sizeof(s->actions[0]));
        s->n_actions--;
        send_cp(s, WRITE_ACTION, cmd, proto_ancs_build_perform_action(uid, action, cmd, sizeof(cmd)));
        return;
    }
    if (s->phase == ANCS_PHASE_NONE && s->n_queue > 0) {
        start_fetch(s, now_ms);
    }
}
