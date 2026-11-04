/*
 * LVGL 9.5.0 configuration for the Paper UI, used by the host build (simulator, golden images).
 * Everything not set here keeps LVGL's default (lv_conf_internal.h). The firmware build uses this
 * same file (LV_CONF_PATH, components/ui_common/CMakeLists.txt). Under ESP-IDF an option that is
 * not set here takes the Kconfig default, which can differ from LVGL's own default, so options
 * that change rendering are pinned below; tools/lvgl_conf_check.py diffs the effective values of
 * both builds and must stay clean.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Native depth RGB565 for the LCD (LCD); the e-paper display sets I1 itself (1 = white, MSB =
 * leftmost pixel) and renders the same pixels as with depth 1: every Paper golden image is unchanged
 * (PLAN D52). */
#define LV_COLOR_DEPTH 16

/* Memory: LVGL's own heap, no OS, no logging (notification text must never reach a log). */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
/* The pool of LVGL's objects and styles. A clock face costs 30-100 objects of about 250 bytes next to the screens' own (the
 * Paper's idle screen alone leaves about 5 KB of 48 KB, measured on the fake board 2026-11-04, D117), so the firmware pool
 * is 128 KB and lives in PSRAM (the static array gets the section attribute; internal RAM is shared with BLE, Wi-Fi and
 * TLS and gains the 48 KB it used to take). Pixels are drawn from internal buffers; only the object structs are read from
 * PSRAM, which an e-paper redraw once a minute and the LCD's partial redraws should not notice.
 * TODO(hw-verify): the LCD's redraw time and the free internal RAM with PSRAM on both boards (checklist 85). The host
 * builds (tests, fake board) use the same size, so the tests see the same limit. ui_face.c checks the pool before it
 * builds a face and falls back to Classic. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY EXT_RAM_BSS_ATTR
#endif
#define LV_MEM_SIZE (128 * 1024U)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_SYSMON 0

#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

/* Software renderer: the formats this UI draws into or from. */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_DRAW_SW_SUPPORT_I1 1
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0

/* Fonts: the UI sets the generated 1-bpp Noto Sans fonts (components/ui_common/fonts) on every
 * label; LVGL's default is the tiny built-in 1-bpp UNSCII 8 so LVGL does not depend on them. */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_UNSCII_8 1
#define LV_FONT_DEFAULT &lv_font_unscii_8
#define LV_USE_FONT_PLACEHOLDER 1

/* Pinned because the ESP-IDF Kconfig default differs from the host's: colour-mix rounding (a 1-bit
 * threshold can flip) and the characters a line may break at (the Kconfig list lacks ']'). */
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"
/* Introspection helpers the UI does not use (on by default only on the host). */
#define LV_USE_OBJ_ID_BUILTIN 0
#define LV_USE_OBJ_PROPERTY_NAME 0

/* LZ4-compressed images (the colour emoji, PLAN D60): the image decoder unpacks a compressed C-array
 * image into a buffer from the LVGL pool for each draw (a 20 px emoji: about 1.2 KB). Needs RAM_LOAD. */
#define LV_USE_LZ4_INTERNAL 1
#define LV_BIN_DECODER_RAM_LOAD 1

/* qrcodegen for the Wi-Fi setup hotspot's QR code (LCD, PLAN D61). The colour layout draws the modules
 * into an A1 canvas itself: the lv_qrcode widget's I1 image is decoded to ARGB8888 (61 KB at 124 px). Unused
 * code is dropped at link time. */
#define LV_USE_QRCODE 1

/* No themes: the UI styles every object itself (deterministic black on white). */
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

#endif /* LV_CONF_H */
