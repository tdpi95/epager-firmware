/* ANCS client helpers: parsing and request building only (no BLE). Apple ANCS spec;
 * byte layouts cross-checked with ESP-IDF's examples/bluetooth/nimble/ble_ancs. Portable C11. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/proto_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PROTO_ANCS_EVENT_ADDED = 0,
    PROTO_ANCS_EVENT_MODIFIED = 1,
    PROTO_ANCS_EVENT_REMOVED = 2,
};
enum {
    PROTO_ANCS_FLAG_SILENT = 1u << 0,
    PROTO_ANCS_FLAG_IMPORTANT = 1u << 1,
    PROTO_ANCS_FLAG_PRE_EXISTING = 1u << 2,
    PROTO_ANCS_FLAG_POSITIVE_ACTION = 1u << 3,
    PROTO_ANCS_FLAG_NEGATIVE_ACTION = 1u << 4,
};
enum {
    PROTO_ANCS_CMD_GET_NOTIF_ATTRS = 0,
    PROTO_ANCS_CMD_GET_APP_ATTRS = 1,
    PROTO_ANCS_CMD_PERFORM_ACTION = 2,
};
enum {
    PROTO_ANCS_ATTR_APP_ID = 0,
    PROTO_ANCS_ATTR_TITLE = 1,     /* needs max_len */
    PROTO_ANCS_ATTR_SUBTITLE = 2,  /* needs max_len */
    PROTO_ANCS_ATTR_MESSAGE = 3,   /* needs max_len */
    PROTO_ANCS_ATTR_MESSAGE_SIZE = 4,
    PROTO_ANCS_ATTR_DATE = 5,
    PROTO_ANCS_ATTR_POSITIVE_ACTION_LABEL = 6,
    PROTO_ANCS_ATTR_NEGATIVE_ACTION_LABEL = 7,
};
enum { PROTO_ANCS_APP_ATTR_DISPLAY_NAME = 0 };
enum { PROTO_ANCS_ACTION_POSITIVE = 0, PROTO_ANCS_ACTION_NEGATIVE = 1 };

typedef struct {
    uint8_t event_id;
    uint8_t event_flags;
    uint8_t category_id;
    uint8_t category_count;
    uint32_t uid;
} proto_ancs_ns_t;

/* Notification Source packet (8 bytes; longer packets accepted). */
proto_status_t proto_ancs_parse_ns(const uint8_t *p, size_t len, proto_ancs_ns_t *out);

typedef struct {
    uint8_t id;
    uint16_t max_len; /* used only for Title/Subtitle/Message */
} proto_ancs_attr_req_t;

/* Control Point commands. Return the length written, 0 if cap is too small. */
size_t proto_ancs_build_get_notif_attrs(uint32_t uid, const proto_ancs_attr_req_t *attrs, size_t n, uint8_t *out,
                                        size_t cap);
size_t proto_ancs_build_get_app_attrs(const uint8_t *app_id, size_t app_id_len, const uint8_t *attr_ids, size_t n,
                                      uint8_t *out, size_t cap);
size_t proto_ancs_build_perform_action(uint32_t uid, uint8_t action_id, uint8_t *out, size_t cap);

/* Data Source reassembly for one outstanding request. */
#define PROTO_ANCS_MAX_ATTRS 8

typedef struct {
    uint8_t command;
    uint32_t uid;
    const uint8_t *app_id; /* GetAppAttributes: caller keeps it alive */
    size_t app_id_len;
    uint8_t attr_ids[PROTO_ANCS_MAX_ATTRS];
    uint8_t n_attrs;
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool done;
    uint16_t attr_off[PROTO_ANCS_MAX_ATTRS];
    uint16_t attr_len[PROTO_ANCS_MAX_ATTRS];
} proto_ancs_ds_t;

void proto_ancs_ds_init_notif(proto_ancs_ds_t *ds, uint32_t uid, const uint8_t *attr_ids, size_t n, uint8_t *buf,
                              size_t cap);
void proto_ancs_ds_init_app(proto_ancs_ds_t *ds, const uint8_t *app_id, size_t app_id_len, const uint8_t *attr_ids,
                            size_t n, uint8_t *buf, size_t cap);
/* Feed one Data Source notification. Returns PROTO_OK (with *complete set when the whole
 * response has arrived), PROTO_MALFORMED (header/order mismatch) or PROTO_TOO_LARGE
 * (capacity). Fragments after completion are ignored. */
proto_status_t proto_ancs_ds_feed(proto_ancs_ds_t *ds, const uint8_t *frag, size_t len, bool *complete);
/* After completion: value of a requested attribute. */
bool proto_ancs_ds_attr(const proto_ancs_ds_t *ds, uint8_t attr_id, const uint8_t **val, uint16_t *len);

/* "yyyyMMdd'T'HHmmSS" (local time, years 2000-2099) -> seconds since 1970-01-01 local.
 * PROTO_MALFORMED for format errors, PROTO_OUT_OF_RANGE for impossible dates. */
proto_status_t proto_ancs_parse_date(const uint8_t *s, size_t len, int64_t *local_epoch_s);

#ifdef __cplusplus
}
#endif
