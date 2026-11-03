/* ota_flash_t on the inactive OTA slot, and the boot-side calls (ESP-IDF only). */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "epager/ota.h"

static const char *TAG = "ota";

static const esp_partition_t *slot;      /* the inactive OTA slot */
static esp_ota_handle_t handle;          /* an update is open when non-zero */
static ota_flash_t flash;

static void close_handle(void)
{
    if (handle != 0) {
        (void)esp_ota_abort(handle);
        handle = 0;
    }
}

static ota_flash_status_t map_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return OTA_FLASH_OK;
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        return OTA_FLASH_BUSY;
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return OTA_FLASH_BAD_IMAGE;
    default:
        ESP_LOGE(TAG, "flash: %s", esp_err_to_name(err));
        return OTA_FLASH_ERROR;
    }
}

/* esp_ota_begin(SEQUENTIAL_WRITES) erases a sector just before the first write into it: a short image does
 * not wait for a whole slot to be erased, and an interrupted one can continue where it stopped. */
static ota_flash_status_t flash_begin(void *ctx)
{
    (void)ctx;
    close_handle();
    return map_err(esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle));
}

static ota_flash_status_t flash_resume(void *ctx, uint32_t offset)
{
    (void)ctx;
    close_handle();
    /* The slot was invalidated by the esp_ota_begin that started this image; resume does not check again. */
    return map_err(esp_ota_resume(slot, OTA_WITH_SEQUENTIAL_WRITES, offset, &handle));
}

static ota_flash_status_t flash_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    return handle != 0 ? map_err(esp_ota_write(handle, data, len)) : OTA_FLASH_ERROR;
}

static ota_flash_status_t flash_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    (void)ctx;
    return esp_partition_read(slot, offset, buf, len) == ESP_OK ? OTA_FLASH_OK : OTA_FLASH_ERROR;
}

/* An image of the other variant would pass the checksum and the signature (same key) and then boot into a
 * firmware for another board: compare the project name first. */
static bool image_is_ours(void)
{
    esp_app_desc_t desc;
    const esp_app_desc_t *run = esp_app_get_description();
    return esp_ota_get_partition_description(slot, &desc) == ESP_OK &&
           strncmp(desc.project_name, run->project_name, sizeof(desc.project_name)) == 0;
}

static ota_end_t flash_end(void *ctx, uint32_t size)
{
    (void)ctx;
    (void)size;
    if (handle == 0) {
        return OTA_END_ERROR;
    }
    if (!image_is_ours()) {
        ESP_LOGW(TAG, "image is not for this firmware");
        close_handle();
        return OTA_END_BAD_IMAGE;
    }
    esp_err_t err = esp_ota_end(handle); /* image checksum and, with signed updates, the signature */
    handle = 0;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        /* The bytes arrived intact (the transfer layer checked their SHA-256), so what fails here is the
         * image structure or the signature block. With signed updates required, say signature. */
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        return OTA_END_BAD_SIGNATURE;
#else
        return OTA_END_BAD_IMAGE;
#endif
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return OTA_END_ERROR;
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition: %s", esp_err_to_name(err));
        return OTA_END_ERROR;
    }
    ESP_LOGI(TAG, "update installed in %s, rebooting", slot->label);
    return OTA_END_OK;
}

static void flash_abort(void *ctx)
{
    (void)ctx;
    close_handle();
}

const ota_flash_t *ota_esp_flash(void)
{
    if (flash.slot_size == 0) {
        slot = esp_ota_get_next_update_partition(NULL);
        if (slot == NULL) {
            ESP_LOGW(TAG, "no second OTA slot: updates are off");
            return NULL;
        }
        flash = (ota_flash_t){.slot_size = slot->size, .begin = flash_begin, .resume = flash_resume,
                              .write = flash_write, .read = flash_read, .end = flash_end, .abort = flash_abort};
    }
    return &flash;
}

bool ota_esp_pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_esp_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "self-check passed: image marked valid");
    } else {
        ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(err));
    }
}

void ota_esp_rollback(void)
{
    ESP_LOGE(TAG, "self-check failed: rolling back");
    /* Fails if there is no valid image to go back to; a plain restart makes the bootloader decide. */
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback: %s", esp_err_to_name(err));
    esp_restart();
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void ota_esp_reboot_in(uint32_t delay_ms)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "ota_reboot"};
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_restart();
        }
    }
    if (esp_timer_start_once(timer, (uint64_t)delay_ms * 1000u) != ESP_OK) {
        esp_restart();
    }
}
