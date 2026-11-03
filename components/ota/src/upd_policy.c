/* When to check for an update, and what the person chose (docs/wifi-ota-plan.md). */
#include <string.h>

#include "epager/upd.h"

#define REC_VERSION 1u
#define UPD_SNOOZE_NO_CLOCK UINT32_MAX
/* ver(1) last_check(4) last_attempt(4) remind_until(4) retries(1) avail_len(1) skip_len(1) avail skip */
#define REC_FIXED 16u

static void put32(uint8_t *p, uint32_t v) { proto_wr32(p, v); }
static uint32_t get32(const uint8_t *p) { return proto_rd32(p); }

static void save(const upd_policy_t *u)
{
    uint8_t b[REC_FIXED + 2 * UPD_VERSION_MAX];
    const size_t al = strlen(u->r.available), sl = strlen(u->r.skipped);
    b[0] = REC_VERSION;
    put32(b + 1, u->r.last_check_utc);
    put32(b + 5, u->r.last_attempt_utc);
    put32(b + 9, u->r.remind_until_utc);
    b[13] = u->r.retries;
    b[14] = (uint8_t)al;
    b[15] = (uint8_t)sl;
    memcpy(b + REC_FIXED, u->r.available, al);
    memcpy(b + REC_FIXED + al, u->r.skipped, sl);
    (void)u->p.kv->set(u->p.kv->ctx, UPD_RECORD_KEY, b, REC_FIXED + al + sl); /* best effort */
}

static bool load(upd_policy_t *u)
{
    uint8_t b[REC_FIXED + 2 * UPD_VERSION_MAX];
    size_t n = 0;
    if (u->p.kv->get(u->p.kv->ctx, UPD_RECORD_KEY, b, sizeof(b), &n) != KV_OK || n < REC_FIXED || b[0] != REC_VERSION) {
        return false;
    }
    const size_t al = b[14], sl = b[15];
    if (al > UPD_VERSION_MAX || sl > UPD_VERSION_MAX || n != REC_FIXED + al + sl) {
        return false;
    }
    u->r.last_check_utc = get32(b + 1);
    u->r.last_attempt_utc = get32(b + 5);
    u->r.remind_until_utc = get32(b + 9);
    u->r.retries = b[13];
    memcpy(u->r.available, b + REC_FIXED, al);
    memcpy(u->r.skipped, b + REC_FIXED + al, sl);
    return true;
}

/* version is newer than the running firmware (an unparsable one counts as newer: the board decides at install). */
static bool newer_than_running(const upd_policy_t *u, const char *version)
{
    int cmp;
    if (!xfer_version_compare(version, strlen(version), u->p.running_version, strlen(u->p.running_version), &cmp)) {
        return true;
    }
    return cmp > 0;
}

void upd_policy_init(upd_policy_t *u, const upd_policy_params_t *p, bool auto_check)
{
    memset(u, 0, sizeof(*u));
    u->p = *p;
    u->auto_check = auto_check;
    u->last_verdict = UPD_UP_TO_DATE;
    if (!load(u)) {
        memset(&u->r, 0, sizeof(u->r));
        return;
    }
    bool changed = false;
    if (u->r.available[0] && !newer_than_running(u, u->r.available)) {
        u->r.available[0] = '\0';
        u->r.remind_until_utc = 0;
        changed = true;
    }
    if (u->r.skipped[0] && !newer_than_running(u, u->r.skipped)) {
        u->r.skipped[0] = '\0';
        changed = true;
    }
    if (changed) {
        save(u);
    }
}

void upd_policy_set_auto_check(upd_policy_t *u, bool on) { u->auto_check = on; }

bool upd_policy_check_due(const upd_policy_t *u, uint32_t now_utc, uint32_t uptime_s)
{
    const upd_record_t *r = &u->r;
    if (!u->auto_check || now_utc == 0 || uptime_s < u->p.first_delay_s) {
        return false;
    }
    const bool day_over = r->last_check_utc == 0 || now_utc < r->last_check_utc ||
                          now_utc - r->last_check_utc >= u->p.period_s + u->p.jitter_s;
    if (!day_over) {
        return false;
    }
    if (r->retries == 0) {
        return true;
    }
    const uint32_t wait = r->retries < u->p.max_tries ? u->p.retry_s : u->p.period_s;
    return now_utc < r->last_attempt_utc || now_utc - r->last_attempt_utc >= wait;
}

void upd_policy_checked(upd_policy_t *u, uint32_t now_utc, upd_verdict_t v, const char *version)
{
    upd_record_t *r = &u->r;
    r->last_check_utc = r->last_attempt_utc = now_utc;
    r->retries = 0;
    u->last_verdict = v;
    if (v == UPD_AVAILABLE && version != NULL && strlen(version) <= UPD_VERSION_MAX) {
        if (strcmp(r->available, version) == 0) {
            if (r->remind_until_utc == UPD_SNOOZE_NO_CLOCK) { /* snoozed while the clock was unknown: from now */
                r->remind_until_utc = now_utc + u->p.snooze_s;
            }
        } else {
            memset(r->available, 0, sizeof(r->available));
            memcpy(r->available, version, strlen(version));
            r->remind_until_utc = 0; /* a different release is news again */
        }
    } else {
        r->available[0] = '\0'; /* nothing to offer now (also: up to date, no image, needs a newer app) */
        r->remind_until_utc = 0;
    }
    save(u);
}

void upd_policy_check_failed(upd_policy_t *u, uint32_t now_utc)
{
    u->r.last_attempt_utc = now_utc;
    if (u->r.retries < 255) {
        u->r.retries++;
    }
    save(u);
}

const char *upd_policy_available(const upd_policy_t *u) { return u->r.available; }

bool upd_policy_skipped(const upd_policy_t *u)
{
    return u->r.available[0] && strcmp(u->r.available, u->r.skipped) == 0;
}

bool upd_policy_mark(const upd_policy_t *u) { return u->r.available[0] && !upd_policy_skipped(u); }

bool upd_policy_prompt_due(const upd_policy_t *u, uint32_t now_utc)
{
    if (!upd_policy_mark(u)) {
        return false;
    }
    const uint32_t until = u->r.remind_until_utc;
    if (until == 0) {
        return true; /* never snoozed: needs no clock */
    }
    if (until == UPD_SNOOZE_NO_CLOCK || now_utc == 0) {
        return false;
    }
    return now_utc >= until || now_utc + u->p.snooze_s < until; /* or the clock went back */
}

void upd_policy_remind_later(upd_policy_t *u, uint32_t now_utc)
{
    /* With no clock the snooze cannot be measured: it starts at the next good check, which needs a clock. */
    u->r.remind_until_utc = now_utc != 0 ? now_utc + u->p.snooze_s : UPD_SNOOZE_NO_CLOCK;
    save(u);
}

void upd_policy_prompt_shown(upd_policy_t *u, uint32_t now_utc) { upd_policy_remind_later(u, now_utc); }

void upd_policy_skip(upd_policy_t *u)
{
    if (u->r.available[0]) {
        memset(u->r.skipped, 0, sizeof(u->r.skipped));
        memcpy(u->r.skipped, u->r.available, strlen(u->r.available));
        save(u);
    }
}

void upd_policy_installed(upd_policy_t *u)
{
    memset(u->r.available, 0, sizeof(u->r.available));
    memset(u->r.skipped, 0, sizeof(u->r.skipped));
    u->r.remind_until_utc = 0;
    save(u);
}
