/*
 * ANCS client state machine (protocol.md 6.2, PLAN D68): the part of the iOS notification path that has no
 * radio in it. The glue (ancs_glue.c) hands it what the iPhone sent (Notification Source and Data Source
 * values, write results) and writes what it asks for to the Control Point; it turns those into
 * npipe_ancs_event() calls. One Control Point request is outstanding at a time. Portable C11, no logging
 * (notification content never leaves except through the pipeline), single-threaded: the app task.
 *
 * Apple's Notification Center Service: UIDs are valid within one session only; a removal for a UID applies
 * at once and cancels a fetch for it; a failed or timed-out fetch drops that notification, never the session.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/npipe.h"
#include "epager/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANCS_QUEUE_MAX 32u      /* notifications waiting for their attributes (iOS replays pre-existing ones) */
#define ANCS_ACTION_MAX 4u      /* negative actions waiting for the Control Point */
#define ANCS_APP_CACHE_MAX 16u  /* app display names kept for the session */
#define ANCS_APP_ID_MAX 64u
#define ANCS_APP_NAME_MAX 64u
#define ANCS_NOTIF_BUF 1536u    /* AppIdentifier, Title 128, Subtitle 128, Message 1024, Date, tuple headers */
#define ANCS_APP_BUF 256u
#define ANCS_CMD_MAX 96u        /* the longest Control Point command */
#define ANCS_TIMEOUT_MS 5000u   /* a request without its whole answer by then is dropped */

typedef struct {
    npipe_t *np;
    /* Write one command to the Control Point (with response). Return false if it cannot be sent now (no
     * link): the request is dropped. The platform later calls ancs_sm_write_done(). */
    bool (*write_cp)(void *ctx, const uint8_t *cmd, size_t len);
    void *ctx;
} ancs_sm_params_t;

typedef enum { ANCS_PHASE_NONE = 0, ANCS_PHASE_NOTIF, ANCS_PHASE_APP } ancs_phase_t;

typedef struct {
    bool used;
    uint8_t id[ANCS_APP_ID_MAX], name[ANCS_APP_NAME_MAX];
    uint8_t id_len, name_len; /* name_len 0: iOS has no name for it (not asked again this session) */
} ancs_app_t;

typedef struct {
    ancs_sm_params_t p;

    proto_ancs_ns_t queue[ANCS_QUEUE_MAX];
    size_t n_queue;
    struct { uint32_t uid; uint8_t action; } actions[ANCS_ACTION_MAX];
    size_t n_actions;
    ancs_app_t apps[ANCS_APP_CACHE_MAX];
    size_t next_evict;

    /* the request in flight */
    ancs_phase_t phase;
    proto_ancs_ns_t cur;
    bool cancelled;       /* a removal for cur.uid arrived while it was being fetched */
    bool write_pending;   /* a Control Point write (any command) has no result yet */
    uint8_t write_kind;   /* what it was: fetch or action */
    uint8_t stale_writes; /* writes given up on after a timeout whose result may still arrive */
    bool ds_complete;
    uint32_t deadline_ms;
    proto_ancs_ds_t ds_notif, ds_app;
    uint8_t buf_notif[ANCS_NOTIF_BUF], buf_app[ANCS_APP_BUF];
    uint8_t app_id[ANCS_APP_ID_MAX];
    size_t app_id_len;

    /* counters for the log (no content) */
    uint32_t n_shown, n_dropped, n_timeouts, n_errors;
} ancs_sm_t;

void ancs_sm_init(ancs_sm_t *s, const ancs_sm_params_t *p);
/* The iPhone's Notification Source is subscribed: a new ANCS session (drops every ANCS notification; iOS
 * re-sends the pre-existing ones). */
void ancs_sm_session_start(ancs_sm_t *s);
/* The link is gone: forget the request in flight and everything queued. Notifications stay on the board. */
void ancs_sm_session_end(ancs_sm_t *s);

void ancs_sm_on_ns(ancs_sm_t *s, uint32_t now_ms, const uint8_t *v, size_t len);
void ancs_sm_on_ds(ancs_sm_t *s, uint32_t now_ms, const uint8_t *v, size_t len);
/* Result of the last Control Point write: 0, or the ATT error code (ANCS: 0xA0-0xA3 are its own errors). */
void ancs_sm_write_done(ancs_sm_t *s, uint32_t now_ms, uint8_t att_status);
/* The user dismissed an iPhone notification on the board: tell iOS (PerformNotificationAction, negative). */
bool ancs_sm_negative_action(ancs_sm_t *s, uint32_t uid);

/* Timeouts and the next request. Call after every input and about once a second. */
void ancs_sm_poll(ancs_sm_t *s, uint32_t now_ms);
/* Work in flight or waiting (the connection wants the fast interval, D68). */
bool ancs_sm_busy(const ancs_sm_t *s);

#ifdef __cplusplus
}
#endif
