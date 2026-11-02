/*
 * LCD P1a bring-up image (docs/MILESTONES.md P1a), built instead of the product with
 * CONFIG_EPAGER_LCD_BRINGUP=y (variants/lcd/main/Kconfig.projbuild). It checks the
 * ostb-xiaozhi-3st through board_api only, with test patterns drawn straight into the LCD (no LVGL
 * yet, that is P1b) and everything else on the console:
 *
 *   volume up, short    next pattern            volume up, long     backlight up 20 %
 *   volume down, short  previous pattern        volume down, long   backlight down 20 %
 *   touch               a dot under the finger (touch pattern), coordinates in the log
 *
 * Patterns: 0 colour bars with a 1-pixel white border and corner markers (top left red, top right
 * green, bottom left blue, bottom right white: orientation, mirroring and offset), 1 gradients
 * (byte order and colour depth), 2-6 solid red, green, blue, white, black, 7 touch canvas, 8 touch
 * targets (9 crosshairs at known points, logged with their positions, for the touch calibration).
 * Also: NVS and the LittleFS assets partition are mounted (partition table check). No BLE (M5), no
 * core (P1b). Notification content does not exist in this image.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "epager/board_api.h"
#include "sdkconfig.h"

#if CONFIG_EPAGER_LCD_BRINGUP

#define TAG "lcd"

#define ASSETS_LABEL "assets"
#define ASSETS_MOUNT "/assets"
#define LOOP_MS 20
#define BATTERY_POLL_MS 5000
#define N_PATTERNS 9
#define PATTERN_TOUCH 7
#define PATTERN_TARGETS 8
#define TARGET_ARM 10
#define DOT 6

static const board_profile_t *b;
static const board_display_t *d;
static board_display_info_t di;
static uint16_t *frame; /* one full frame in PSRAM, RGB565 CPU order */
static QueueHandle_t keys;
static int pattern, backlight = 60;

typedef struct {
    board_button_t btn;
    board_press_t press;
} key_ev_t;

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t bl)
{
    return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | bl >> 3);
}

/* ---------------------------------------------------------------- storage (partition table check) */

static bool init_storage(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The first boot finds the stock firmware's NVS layout: start clean (the stock backup keeps it). */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    esp_vfs_littlefs_conf_t conf = {.base_path = ASSETS_MOUNT, .partition_label = ASSETS_LABEL,
                                    .format_if_mount_failed = true};
    esp_err_t fs = esp_vfs_littlefs_register(&conf);
    size_t total = 0, used = 0;
    if (fs == ESP_OK && esp_littlefs_info(ASSETS_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "assets: %u of %u bytes used", (unsigned)used, (unsigned)total);
    }
    ESP_LOGI(TAG, "storage: nvs %s, assets %s", esp_err_to_name(err), esp_err_to_name(fs));
    return err == ESP_OK && fs == ESP_OK;
}

/* ---------------------------------------------------------------- patterns */

static void draw_bars(void)
{
    static const uint8_t c[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
    const int w = di.width, h = di.height, m = 16;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *k = c[x * 8 / w];
            uint16_t px = rgb(k[0], k[1], k[2]);
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                px = 0xFFFF;
            } else if (x >= 2 && x < 2 + m && y >= 2 && y < 2 + m) {
                px = rgb(255, 0, 0);
            } else if (x >= w - 2 - m && x < w - 2 && y >= 2 && y < 2 + m) {
                px = rgb(0, 255, 0);
            } else if (x >= 2 && x < 2 + m && y >= h - 2 - m && y < h - 2) {
                px = rgb(0, 0, 255);
            } else if (x >= w - 2 - m && x < w - 2 && y >= h - 2 - m && y < h - 2) {
                px = 0xFFFF;
            }
            frame[y * w + x] = px;
        }
    }
}

static void draw_gradients(void)
{
    const int w = di.width, h = di.height;
    for (int y = 0; y < h; y++) {
        const int band = y * 4 / h;
        for (int x = 0; x < w; x++) {
            const uint8_t v = (uint8_t)(x * 255 / (w - 1));
            frame[y * w + x] = band == 0 ? rgb(v, 0, 0) : band == 1 ? rgb(0, v, 0) : band == 2 ? rgb(0, 0, v)
                                                                                              : rgb(v, v, v);
        }
    }
}

static void draw_solid(uint16_t px)
{
    for (int i = 0; i < di.width * di.height; i++) {
        frame[i] = px;
    }
}

static void draw_touch_canvas(void)
{
    const int w = di.width, h = di.height;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            frame[y * w + x] = (x % 37 == 0 || y % 40 == 0) ? rgb(48, 48, 48) : 0x0000;
        }
    }
}

/* Crosshairs 24 px from the edges, at the edge middles and the centre. */
static const uint16_t TARGET_X[3] = {24, 148, 271}, TARGET_Y[3] = {24, 120, 215};

static void draw_targets(void)
{
    const int w = di.width;
    draw_solid(0x0000);
    for (int ty = 0; ty < 3; ty++) {
        for (int tx = 0; tx < 3; tx++) {
            const int cx = TARGET_X[tx], cy = TARGET_Y[ty];
            for (int i = -TARGET_ARM; i <= TARGET_ARM; i++) {
                frame[cy * w + cx + i] = rgb(255, 255, 0);
                frame[(cy + i) * w + cx] = rgb(255, 255, 0);
            }
        }
    }
    for (int ty = 0; ty < 3; ty++) {
        ESP_LOGI(TAG, "target row %d: (%u,%u) (%u,%u) (%u,%u)", ty, TARGET_X[0], TARGET_Y[ty], TARGET_X[1],
                 TARGET_Y[ty], TARGET_X[2], TARGET_Y[ty]);
    }
}

static void show_pattern(void)
{
    static const char *names[N_PATTERNS] = {"colour bars", "gradients", "red", "green", "blue", "white", "black",
                                            "touch canvas", "touch targets"};
    switch (pattern) {
    case 0: draw_bars(); break;
    case 1: draw_gradients(); break;
    case 2: draw_solid(rgb(255, 0, 0)); break;
    case 3: draw_solid(rgb(0, 255, 0)); break;
    case 4: draw_solid(rgb(0, 0, 255)); break;
    case 5: draw_solid(0xFFFF); break;
    case 6: draw_solid(0x0000); break;
    case 7: draw_touch_canvas(); break;
    default: draw_targets(); break;
    }
    const int64_t t0 = esp_timer_get_time();
    d->write_area(d->ctx, 0, 0, di.width, di.height, frame);
    ESP_LOGI(TAG, "pattern %d (%s): full frame written in %lld us", pattern, names[pattern],
             (long long)(esp_timer_get_time() - t0));
}

/* ---------------------------------------------------------------- input */

/* esp_timer task: only queue the event. */
static void on_button(void *user, board_button_t btn, board_press_t press)
{
    (void)user;
    key_ev_t ev = {btn, press};
    xQueueSend(keys, &ev, 0);
}

static void handle_key(const key_ev_t *ev)
{
    const bool up = ev->btn == BOARD_BTN_UP;
    ESP_LOGI(TAG, "key %s %s", up ? "UP" : ev->btn == BOARD_BTN_DOWN ? "DOWN" : "?",
             ev->press == BOARD_PRESS_LONG ? "long" : "short");
    if (ev->press == BOARD_PRESS_SHORT) {
        pattern = (pattern + (up ? 1 : N_PATTERNS - 1)) % N_PATTERNS;
        show_pattern();
    } else if (d->set_backlight != NULL) {
        backlight += up ? 20 : -20;
        backlight = backlight > 100 ? 100 : backlight < 0 ? 0 : backlight;
        d->set_backlight(d->ctx, (uint8_t)backlight);
        ESP_LOGI(TAG, "backlight %d %%", backlight);
    }
}

static void poll_touch(void)
{
    static bool was_down;
    static board_touch_t last; /* the up event carries no position: report where the finger was */
    board_touch_t t;
    if (b->read_touch == NULL || !b->read_touch(b->ctx, &t)) {
        return;
    }
    if (t.pressed && !was_down) {
        ESP_LOGI(TAG, "touch down at %u,%u", t.x, t.y);
    } else if (!t.pressed && was_down) {
        ESP_LOGI(TAG, "touch up at %u,%u", last.x, last.y);
    }
    was_down = t.pressed;
    if (t.pressed) {
        last = t;
    }
    if (t.pressed && (pattern == PATTERN_TOUCH || pattern == PATTERN_TARGETS) && t.x < di.width && t.y < di.height) {
        static uint16_t dot[DOT * DOT];
        for (int i = 0; i < DOT * DOT; i++) {
            dot[i] = 0xFFFF;
        }
        const uint16_t x = t.x >= DOT / 2 ? t.x - DOT / 2 : 0, y = t.y >= DOT / 2 ? t.y - DOT / 2 : 0;
        d->write_area(d->ctx, x, y, DOT, DOT, dot); /* clipped at the edges by the driver */
    }
}

static void poll_battery(void)
{
    uint8_t pct = 0, ext = 0;
    if (b->read_battery != NULL && !b->read_battery(b->ctx, &pct, &ext)) {
        ESP_LOGW(TAG, "battery not readable");
    }
    /* The board logs the raw reading and the level when they change. */
}

/* ---------------------------------------------------------------- main */

void bringup_main(void)
{
    b = board_get();
    d = b->display;
    bool board_ok = b->init == NULL || b->init(b->ctx);
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "LCD %s (P1a bring-up), board %s, IDF %s", app->version, b->board_id, app->idf_ver);

    bool storage_ok = init_storage();
    d->get_info(d->ctx, &di);
    frame = heap_caps_malloc((size_t)di.width * di.height * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool lcd_ok = frame != NULL && d->write_area != NULL && d->init(d->ctx);
    keys = xQueueCreate(8, sizeof(key_ev_t));
    if (b->set_button_handler != NULL && keys != NULL) {
        b->set_button_handler(b->ctx, on_button, NULL);
    }
    ESP_LOGI(TAG, "boot: board %s, storage %s, lcd %s (%ux%u, %u bpp), touch %s", board_ok ? "ok" : "FAIL",
             storage_ok ? "ok" : "FAIL", lcd_ok ? "ok" : "FAIL", di.width, di.height, di.color_depth,
             b->read_touch != NULL ? "ok" : "absent");
    ESP_LOGI(TAG, "heap: %u bytes internal free, %u bytes PSRAM free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!lcd_ok) {
        return;
    }
    show_pattern();
    if (d->set_backlight != NULL) {
        d->set_backlight(d->ctx, (uint8_t)backlight); /* only now: the panel shows a real frame */
    }

    uint32_t last_battery = 0;
    bool stack_logged = false;
    for (;;) {
        key_ev_t ev;
        while (xQueueReceive(keys, &ev, 0) == pdTRUE) {
            handle_key(&ev);
        }
        poll_touch();
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_battery >= BATTERY_POLL_MS) {
            last_battery = now;
            poll_battery();
        }
        if (!stack_logged && now > 10000) {
            stack_logged = true;
            ESP_LOGI(TAG, "main task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

#endif
