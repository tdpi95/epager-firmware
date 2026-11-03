/*
 * Firmware updates from the release manifest over Wi-Fi (docs/wifi-ota-plan.md, M6c). Portable C11, no ESP-IDF
 * headers: the net task supplies the HTTP transport, the main loop owns the policy.
 *  - upd_decide: the board's reading of manifest.json (contracts/releases.md, release-manifest.schema.json) and
 *    its rules for offering an image.
 *  - upd_policy: when to check (once a day), what the person chose (install / remind later / skip a version)
 *    and the small NVS record that keeps it across restarts.
 *  - upd_download: stream one image from the transport into a firmware sink, hashing as it goes, resuming a
 *    dropped connection with a Range request.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/kv_storage.h"
#include "epager/proto.h"
#include "epager/xfer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPD_VERSION_MAX 32u
#define UPD_FILE_MAX 128u
#define UPD_IMAGES_MAX 8u /* images read from a manifest; more are ignored */

/* ---------------------------------------------------------------- manifest */

typedef enum {
    UPD_UP_TO_DATE = 0, /* the release is not newer than the running firmware */
    UPD_AVAILABLE,      /* a newer image for this board: see upd_image_t */
    UPD_INVALID,        /* the manifest does not follow releases.md (also a truncated download) */
    UPD_NO_IMAGE,       /* a valid manifest without an image for this board */
    UPD_VARIANT_MISMATCH,
    UPD_TOO_LARGE,      /* the image does not fit the OTA slot */
    UPD_CONTRACT_MAJOR, /* the release speaks another contract major: the companion app would not understand it */
    UPD_PRERELEASE,     /* a pre-release, not offered unless allowed (developer URL) */
} upd_verdict_t;

typedef struct {
    char version[UPD_VERSION_MAX + 1];
    char file[UPD_FILE_MAX + 1];
    uint32_t size;
    uint8_t sha256[PROTO_SHA256_LEN];
} upd_image_t;

typedef struct {
    const char *board_id;           /* CAPABILITIES.board_id */
    uint8_t variant;                /* PROTO_VARIANT_* */
    const char *running_version;
    const char *contract_version;   /* the board's own: PROTO_CONTRACT_VERSION */
    uint32_t slot_size;
    bool allow_prerelease;
} upd_board_t;

/* json: the manifest (len bytes, not NUL-terminated). out is filled for UPD_AVAILABLE, and its version also for
 * UPD_UP_TO_DATE, UPD_TOO_LARGE, UPD_CONTRACT_MAJOR and UPD_PRERELEASE (what the release is). Rules in order:
 * invalid, no image for board_id, variant mismatch, too large, contract major, pre-release, not newer. */
upd_verdict_t upd_decide(const char *json, size_t len, const upd_board_t *board, upd_image_t *out);

/* The only hosts the board talks to for an update (the manifest URL and every redirect): https on github.com
 * or a subdomain of githubusercontent.com, no user name, no port but 443. */
bool upd_url_allowed(const char *url);
/* The URL of the image file of a release. For the "latest" manifest URL
 * (https://github.com/OWNER/REPO/releases/latest/download/manifest.json) it names the tag, so the file always
 * belongs to the version the manifest named: .../releases/download/v<version>/<file>. Any other manifest URL
 * (a test release) gives the file next to the manifest. False if the URL has neither shape or out is too small. */
bool upd_image_url(const char *manifest_url, const char *version, const char *file, char *out, size_t cap);

/* ---------------------------------------------------------------- policy */

#define UPD_RECORD_KEY "upd"

typedef struct {
    const kv_storage_t *kv;
    const char *running_version;
    uint32_t jitter_s;      /* added to every period (0..1800 s, per device) */
    uint32_t period_s;      /* between good checks: 86400 */
    uint32_t retry_s;       /* after a failed check: 3600 */
    uint8_t max_tries;      /* failed checks in a row before waiting a whole period: 6 */
    uint32_t first_delay_s; /* no check earlier than this after boot: 3600 */
    uint32_t snooze_s;      /* "remind later": 86400 */
} upd_policy_params_t;

typedef struct {
    uint32_t last_check_utc;   /* the last good check, 0 = never */
    uint32_t last_attempt_utc; /* the last try, good or not */
    uint32_t remind_until_utc; /* the prompt stays away until then, 0 = not snoozed */
    uint8_t retries;           /* failed tries since the last good check */
    char available[UPD_VERSION_MAX + 1]; /* the newest version found, "" = none */
    char skipped[UPD_VERSION_MAX + 1];   /* the version the person skipped, "" = none */
} upd_record_t;

typedef struct {
    upd_policy_params_t p;
    upd_record_t r;
    bool auto_check;
    upd_verdict_t last_verdict; /* of the last conclusive check, RAM only (the page's state line) */
} upd_policy_t;

/* Loads the record. A version that is not newer than the running firmware (it was installed, maybe over BLE) is
 * forgotten, and so is a skip that is no longer ahead of it. */
void upd_policy_init(upd_policy_t *u, const upd_policy_params_t *p, bool auto_check);
void upd_policy_set_auto_check(upd_policy_t *u, bool on);

/* A daily check is due. now_utc 0 = the clock is not set (nothing is due then); uptime_s since boot. */
bool upd_policy_check_due(const upd_policy_t *u, uint32_t now_utc, uint32_t uptime_s);
/* A check got an answer. version is the release's version (for UPD_AVAILABLE, and for the others when known).
 * UPD_INVALID is not an answer: use upd_policy_check_failed. */
void upd_policy_checked(upd_policy_t *u, uint32_t now_utc, upd_verdict_t v, const char *version);
/* A check could not finish (no network, GitHub unreachable, bad manifest). */
void upd_policy_check_failed(upd_policy_t *u, uint32_t now_utc);

/* The newest version found ("" = none), whether the person skipped it, and the status-bar mark. */
const char *upd_policy_available(const upd_policy_t *u);
bool upd_policy_skipped(const upd_policy_t *u);
bool upd_policy_mark(const upd_policy_t *u); /* available and not skipped */
/* The prompt should be shown now: available, not skipped, not snoozed. */
bool upd_policy_prompt_due(const upd_policy_t *u, uint32_t now_utc);
/* The prompt was put on screen. It counts as "remind later" already: leaving it unanswered brings it back
 * after the snooze time and not at once. */
void upd_policy_prompt_shown(upd_policy_t *u, uint32_t now_utc);
void upd_policy_remind_later(upd_policy_t *u, uint32_t now_utc);
void upd_policy_skip(upd_policy_t *u);
/* The update is installed (the board is about to restart): the record is cleared. */
void upd_policy_installed(upd_policy_t *u);

/* ---------------------------------------------------------------- download */

typedef struct {
    int status;            /* HTTP status after redirects */
    uint32_t content_len;  /* body length, UINT32_MAX = unknown */
    uint32_t range_first;  /* 206: first byte of the body */
    uint32_t range_total;  /* 206: size of the whole file, UINT32_MAX = unknown */
} upd_http_head_t;

typedef struct {
    /* Start a GET of the image from byte `offset` (a Range request when offset > 0). 0 = got a status line. */
    int (*open)(void *ctx, uint32_t offset, upd_http_head_t *head);
    /* >0 bytes read, 0 end of the body, <0 error. */
    int (*read)(void *ctx, void *buf, size_t cap);
    void (*close)(void *ctx);
    void *ctx;
} upd_http_t;

typedef enum {
    UPD_DL_OK,         /* installed: the board boots the new image after the next restart */
    UPD_DL_NETWORK,    /* the connection kept failing, or the server answered with an error */
    UPD_DL_BAD_SIZE,   /* more or fewer bytes than the manifest says */
    UPD_DL_HASH,       /* the bytes are not the manifest's image */
    UPD_DL_CANCELLED,
    UPD_DL_SINK,       /* the sink refused: see sink_status (BUSY, NO_SPACE, INTERNAL, ...) */
    UPD_DL_REJECTED,   /* the image check failed at the end: IMAGE_INVALID or SIGNATURE_INVALID in sink_status */
} upd_dl_result_t;

typedef struct {
    const xfer_fw_sink_t *sink;
    upd_http_t http;
    uint32_t size;                         /* from the manifest */
    uint8_t sha256[PROTO_SHA256_LEN];      /* from the manifest */
    uint8_t max_resumes;                   /* reconnects before giving up: 3 */
    uint8_t *buf;                          /* caller's block buffer, 4 KiB */
    size_t buf_len;
    void (*progress)(void *ctx, uint32_t done, uint32_t total);
    bool (*cancelled)(void *ctx);
    void *ctx;
} upd_download_params_t;

upd_dl_result_t upd_download(const upd_download_params_t *p, proto_status_t *sink_status);

#ifdef __cplusplus
}
#endif
