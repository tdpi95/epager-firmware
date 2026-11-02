/*
 * Portable OTA decisions and states (PLAN D11/D12, protocol.md 7): version/downgrade check,
 * battery gate, progress, and the boot-time pending-verify / self-check / rollback state machine.
 * The ESP-IDF glue (components/ota, M6) performs the actions this module asks for through
 * on_event (reboot, esp_ota_mark_app_valid_cancel_rollback, rollback). Portable C11.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/kv_storage.h"
#include "epager/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XFER_OTA_KV_KEY "ota_pending"
#define XFER_OTA_VERSION_MAX 32u

typedef enum {
    XFER_OTA_EV_PROGRESS,   /* pct: firmware bytes received, for the display */
    XFER_OTA_EV_REBOOT,     /* the new image is the boot partition: reboot in about 1 s */
    XFER_OTA_EV_MARK_VALID, /* self-check passed: mark the running image valid */
    XFER_OTA_EV_ROLLBACK,   /* self-check failed or timed out: mark invalid and reboot */
    XFER_OTA_EV_STATE,      /* state (DEVICE_STATUS.ota_state) changed */
} xfer_ota_event_kind_t;

typedef struct {
    xfer_ota_event_kind_t kind;
    uint8_t pct;   /* PROGRESS */
    uint8_t state; /* STATE: PROTO_OTA_STATE_* */
} xfer_ota_event_t;

typedef struct {
    const char *running_version;   /* MAJOR.MINOR.PATCH[-pre][+build] */
    bool allow_downgrade;          /* development builds only */
    uint8_t low_battery_pct;       /* refuse below this charge (Paper: 30) */
    /* NULL: no battery reader, the gate is skipped. Returns false if the level cannot be read.
     * external_power: PROTO_EXTERNAL_POWER_*. */
    bool (*read_battery)(void *ctx, uint8_t *pct, uint8_t *external_power);
    const kv_storage_t *kv;        /* pending-update record */
    uint32_t required_checks;      /* self-check bits that must all pass after an update */
    uint32_t verify_timeout_ms;    /* roll back if they have not passed by then */
    void (*on_event)(void *ctx, const xfer_ota_event_t *ev);
    void *ctx;
} xfer_ota_params_t;

typedef struct {
    xfer_ota_params_t p;
    uint8_t state;          /* PROTO_OTA_STATE_* */
    uint8_t last_pct;
    bool rollback_requested;
    uint32_t passed;
    uint32_t verify_start_ms;
} xfer_ota_t;

void xfer_ota_init(xfer_ota_t *o, const xfer_ota_params_t *p);

/* XFER_BEGIN gates (protocol.md 7.2 steps 6-7): VERSION_REJECTED, LOW_BATTERY or OK. */
proto_status_t xfer_ota_check_begin(xfer_ota_t *o, const char *version, size_t len, bool has_version);
proto_status_t xfer_ota_check_battery(xfer_ota_t *o);
void xfer_ota_progress(xfer_ota_t *o, uint32_t received, uint32_t total);
/* XFER_FINISH succeeded: remember the version we expect to boot, ask for a reboot. */
void xfer_ota_installed(xfer_ota_t *o, const char *version, size_t len);

/* At boot. image_pending_verify: the bootloader started a new image that is not yet marked valid. */
void xfer_ota_boot(xfer_ota_t *o, bool image_pending_verify, uint32_t now_ms);
void xfer_ota_check_passed(xfer_ota_t *o, uint32_t check_bits);
void xfer_ota_check_failed(xfer_ota_t *o);
void xfer_ota_tick(xfer_ota_t *o, uint32_t now_ms);
uint8_t xfer_ota_state(const xfer_ota_t *o);

/* Semver precedence (semver.org 11; build metadata ignored; numeric parts and numeric pre-release
 * identifiers up to 9 digits; equal numbers written differently are ordered by their text).
 * Returns false if either version is invalid; otherwise *cmp is -1, 0 or 1. */
bool xfer_version_compare(const char *a, size_t alen, const char *b, size_t blen, int *cmp);

#ifdef __cplusplus
}
#endif
