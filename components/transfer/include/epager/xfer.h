/*
 * Transfer state machine for assets and firmware (protocol.md 7): BEGIN / CHUNK / ACK / STATUS /
 * FINISH / ABORT, asset storage requests, resume journal, windowed acks, prefix CRC-32, SHA-256
 * verification, atomic asset install. Platform access: kv_storage (journal), blob_storage (assets),
 * xfer_fw_sink_t (firmware). Portable C11; single-threaded (the core task owns it).
 * Scenario vectors: contracts/test-vectors/transfer.json.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/blob_storage.h"
#include "epager/kv_storage.h"
#include "epager/proto.h"
#include "epager/xfer_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XFER_ID_MAX 32u
#define XFER_VERSION_MAX 32u
#define XFER_JOURNAL_KEY "journal"
#define XFER_ASSET_HEADER_LEN 16u /* asset-formats.md container */

/* Firmware destination (esp_ota_* on the board, fw_mem on the host). Status codes are protocol
 * status values. */
typedef struct {
    uint32_t slot_size;
    proto_status_t (*begin)(void *ctx, uint32_t size);                   /* start a new image */
    proto_status_t (*resume)(void *ctx, uint32_t size, uint32_t offset); /* continue after a reboot */
    proto_status_t (*write)(void *ctx, uint32_t offset, const void *data, size_t len);
    proto_status_t (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    /* Image and signature check, then make it the boot image: OK, IMAGE_INVALID or SIGNATURE_INVALID. */
    proto_status_t (*finish)(void *ctx, uint32_t size);
    void (*abort)(void *ctx);
    void *ctx;
} xfer_fw_sink_t;

/* One CAPABILITIES.asset_support entry. */
typedef struct {
    uint8_t asset_type;
    uint8_t format;
    uint32_t max_size;
    uint16_t width, height;               /* exact image size (app_icon); 0: none, images match the display */
    /* Local limits, not advertised: the bytes all installed assets of this type may take together and how many there
     * may be (clock faces, M8b); an install that replaces an asset does not count the old one. 0 = no limit. */
    uint32_t budget;
    uint16_t max_count;
} xfer_asset_support_t;

typedef struct {
    const kv_storage_t *kv;               /* resume journal (required) */
    const blob_storage_t *blob;           /* NULL: no asset support */
    const xfer_asset_support_t *assets;
    size_t n_assets;
    uint16_t display_w, display_h;        /* image assets without a fixed size must match the display */
    const xfer_fw_sink_t *fw;             /* NULL: no firmware updates */
    xfer_ota_t *ota;                      /* required with fw */
    uint16_t max_chunk;                   /* data bytes per chunk the link carries (<= chunk_data_max) */
    uint32_t ack_window;
    uint32_t journal_every;               /* bytes between journal writes */
    void (*emit)(void *ctx, const uint8_t *msg, size_t len); /* XFER_ACK events (Transfer channel) */
    void (*on_asset)(void *ctx, uint8_t asset_type);         /* an asset was installed or deleted; may be NULL */
    /* The content check of an asset that passed the generic header check, at XFER_FINISH (clock faces: the whole file
     * is parsed). `blob_name` is the stored file; return PROTO_OK or PROTO_IMAGE_INVALID. May be NULL. */
    proto_status_t (*check_asset)(void *ctx, uint8_t asset_type, const char *id, size_t id_len, const char *blob_name,
                                  uint32_t size);
    void *ctx;
} xfer_params_t;

typedef struct {
    uint8_t kind, asset_type, id_len, ver_len;
    bool has_origin;                      /* the origin of the BEGIN request (not journaled: the one that completes wins) */
    uint8_t origin;                       /* PROTO_ASSET_ORIGIN_* */
    char id[XFER_ID_MAX + 1];
    char ver[XFER_VERSION_MAX + 1];
    uint32_t size, crc32;
    uint8_t sha256[PROTO_SHA256_LEN];
} xfer_target_t;

typedef struct {
    xfer_params_t p;
    bool has_session;
    struct {
        xfer_target_t t;
        uint8_t handle, state;            /* state: PROTO_XFER_STATE_* */
        uint32_t offset, crc, last_ack, journaled;
    } s;
    uint8_t next_handle;
} xfer_t;

/* Boot: removes temporary and version files left behind by an interrupted transfer or install. */
void xfer_init(xfer_t *x, const xfer_params_t *p);
/* The link dropped: the session ends, its offset is journaled for a later resume. */
void xfer_on_disconnect(xfer_t *x);
/* Valid Transfer-channel requests. Writes the response into w (cap >= the channel's max_message);
 * returns false if no response is due (XFER_CHUNK) or the type is not a transfer request. */
bool xfer_handle(xfer_t *x, const proto_msg_t *req, proto_writer_t *w);
/* An XFER_CHUNK that failed validation with st: answered with an XFER_ACK (protocol.md 7.3). */
void xfer_chunk_invalid(xfer_t *x, const proto_msg_t *req, proto_status_t st);

/* Name of an installed asset in blob storage ("<type>_<id>"), for readers such as the UI. */
void xfer_asset_blob_name(uint8_t asset_type, const char *id, size_t id_len, char out[BLOB_NAME_MAX + 1]);

/* Delete an installed asset and its version file, as ASSET_DELETE does (on_asset is called). OK, NOT_FOUND,
 * UNSUPPORTED (no asset storage) or INTERNAL. The core uses it to drop the icon of an app that leaves the
 * seen list (protocol.md 6.4). */
proto_status_t xfer_asset_remove(const xfer_t *x, uint8_t asset_type, const char *id, size_t id_len);

#define XFER_APP_ICON_ID_LEN 32u /* lower-case hex of the first 16 bytes of SHA-256(app_id) */
/* Asset id of an app's icon (asset-formats.md app_icon): writes XFER_APP_ICON_ID_LEN chars + NUL. */
void xfer_app_icon_id(const char *app_id, size_t len, char out[XFER_APP_ICON_ID_LEN + 1]);

/* ---------------------------------------------------------------- in-memory firmware sink (host, fake board) */

typedef struct {
    xfer_fw_sink_t iface;
    uint8_t *buf;                  /* slot_size bytes; survives a simulated reboot like flash */
    uint32_t written;              /* high-water mark since begin(); resume() needs offset <= written */
    proto_status_t image_status;   /* what finish() returns (default OK) */
    proto_status_t begin_status;   /* what begin() returns (default OK): BUSY = another update is writing */
    bool boot_set;                 /* finish() accepted the image */
    int32_t *power;                /* shared power budget (see kv_mem): each write consumes one unit */
} fw_mem_t;

void fw_mem_init(fw_mem_t *m, uint8_t *buf, uint32_t slot_size);

#ifdef __cplusplus
}
#endif
