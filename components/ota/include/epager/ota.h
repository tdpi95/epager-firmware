/*
 * Firmware OTA glue (M6, PLAN D11/D12). Two halves:
 *  - ota_sink (portable, host-tested): the xfer_fw_sink_t the transfer state machine writes the image into. It
 *    keeps the write position, refuses anything but sequential writes and maps flash results to protocol
 *    status codes. The flash itself is an ota_flash_t.
 *  - ota_esp (ESP-IDF only): the ota_flash_t on the inactive OTA slot (esp_ota_*, signature check in
 *    esp_ota_end) and the boot-side calls: is the running image waiting for its self-check, mark it valid,
 *    roll back, reboot.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epager/xfer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_FLASH_OK,
    OTA_FLASH_NO_SPACE,
    OTA_FLASH_BUSY,      /* the running image has not passed its self-check yet: no update now */
    OTA_FLASH_BAD_IMAGE, /* the data cannot be an image of this firmware (write: wrong first byte) */
    OTA_FLASH_ERROR,
} ota_flash_status_t;

typedef enum {
    OTA_END_OK,            /* verified and set as the boot image */
    OTA_END_BAD_IMAGE,     /* not an image of this firmware, or its checksum fails */
    OTA_END_BAD_SIGNATURE, /* signature missing or not from the trusted key */
    OTA_END_ERROR,
} ota_end_t;

/* The inactive OTA slot. Writes are sequential: begin() or resume() sets the position, write() appends. */
typedef struct {
    uint32_t slot_size;
    ota_flash_status_t (*begin)(void *ctx);                    /* new image from offset 0 */
    ota_flash_status_t (*resume)(void *ctx, uint32_t offset); /* continue an earlier image at offset */
    ota_flash_status_t (*write)(void *ctx, const void *data, size_t len);
    ota_flash_status_t (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    ota_end_t (*end)(void *ctx, uint32_t size);                /* verify and make it the boot image */
    void (*abort)(void *ctx);
    void *ctx;
} ota_flash_t;

/* Who is writing the image. One writer at a time: whoever starts an image (begin or resume) owns the sink until
 * it finishes or is aborted; the other path gets BUSY and cannot touch the image. */
typedef enum { OTA_OWNER_NONE = 0, OTA_OWNER_BLE, OTA_OWNER_WIFI } ota_owner_t;

typedef struct ota_sink ota_sink_t;

typedef struct {
    ota_sink_t *sink;
    ota_owner_t who;
} ota_view_t;

struct ota_sink {
    xfer_fw_sink_t iface; /* for the transfer state machine (BLE) */
    xfer_fw_sink_t wifi;  /* the same flash for the Wi-Fi download (upd_download), possibly from another task */
    const ota_flash_t *flash;
    uint32_t written;     /* the image is complete up to here */
    bool open;
    atomic_uchar owner;   /* ota_owner_t */
    ota_view_t views[2];
};

void ota_sink_init(ota_sink_t *s, const ota_flash_t *flash);
/* The BLE link is gone: the BLE session stops owning the sink (the image stays; a later BLE resume claims it
 * again). The Wi-Fi download is not affected. */
void ota_sink_ble_gone(ota_sink_t *s);
/* Some session owns the sink. */
bool ota_sink_busy(const ota_sink_t *s);

/* ---------------------------------------------------------------- ESP-IDF side (ota_esp.c) */

/* The inactive slot as a flash, or NULL if the partition table has no second OTA slot. */
const ota_flash_t *ota_esp_flash(void);
/* The bootloader started this image for the first time and nobody has marked it valid yet. */
bool ota_esp_pending_verify(void);
/* The self-check passed: keep this image (esp_ota_mark_app_valid_cancel_rollback). */
void ota_esp_mark_valid(void);
/* The self-check failed: mark this image invalid and boot the previous one. Does not return normally. */
void ota_esp_rollback(void);
/* Restart the chip after delay_ms (the answer to XFER_FINISH still has to leave). */
void ota_esp_reboot_in(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
