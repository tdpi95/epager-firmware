/* Built-in notification and status-bar icons (private to ui_common). Generated data: tools/assets/icon_gen.py. */
#pragma once

#include <stdint.h>

#include "lvgl.h"

#define UI_ICON_SIZE 24u        /* width = height; the Paper's asset_support width/height for app_icon */
#define UI_ICON_BYTES 72u       /* mono1 payload: 3 bytes per row, 24 rows */
#define UI_BUILTIN_ICON_COUNT 12u /* registry enum `category`: index = value, unknown values use 0 (other) */

extern const uint8_t ui_builtin_icons[UI_BUILTIN_ICON_COUNT][UI_ICON_BYTES];

#define UI_STATUS_ICON_SIZE 16u
#define UI_STATUS_ICON_BYTES 32u  /* mono1: 2 bytes per row, 16 rows */

typedef enum {
    UI_STATUS_ICON_BLE_CONNECTED = 0,
    UI_STATUS_ICON_BLE_DISCONNECTED,
    UI_STATUS_ICON_WIFI,
    UI_STATUS_ICON_WIFI_FAILED,
    UI_STATUS_ICON_CHARGING,
    UI_STATUS_ICON_QUIET,  /* a crescent moon: quiet hours apply (PLAN D126) */
    UI_STATUS_ICON_COUNT,
} ui_status_icon_t;

extern const uint8_t ui_status_icons[UI_STATUS_ICON_COUNT][UI_STATUS_ICON_BYTES];

/* Colour weather icons (LCD idle screen), RGB565A8. */
#define UI_WEATHER_ICON_SIZE 32u

typedef enum {
    UI_WEATHER_ICON_SUN = 0,
    UI_WEATHER_ICON_MOON,
    UI_WEATHER_ICON_SUN_SMALL_CLOUD,
    UI_WEATHER_ICON_SUN_CLOUD,
    UI_WEATHER_ICON_CLOUD,
    UI_WEATHER_ICON_FOG,
    UI_WEATHER_ICON_RAIN,
    UI_WEATHER_ICON_HEAVY_RAIN,
    UI_WEATHER_ICON_SUN_RAIN,
    UI_WEATHER_ICON_SNOW,
    UI_WEATHER_ICON_THUNDER,
    UI_WEATHER_ICON_COUNT,
} ui_weather_icon_t;

extern const lv_image_dsc_t ui_weather_icons[UI_WEATHER_ICON_COUNT];

/* The Paper's idle screen icons (PLAN D78), mono1 (bit 1 = white): the same eleven weather conditions at
 * 32x32, a thermometer and a humidity drop at 24x24, an umbrella at 24x24 for the chance of rain. */
#define UI_MONO_WX_SIZE 32u
#define UI_MONO_ENV_SIZE 24u
#define UI_MONO_RAIN_SIZE 24u
enum { UI_MONO_ENV_THERMOMETER = 0, UI_MONO_ENV_HUMIDITY };
extern const uint8_t ui_mono_wx_icons[UI_WEATHER_ICON_COUNT][UI_MONO_WX_SIZE * UI_MONO_WX_SIZE / 8u];
extern const uint8_t ui_mono_env_icons[2][UI_MONO_ENV_SIZE * UI_MONO_ENV_SIZE / 8u];
extern const uint8_t ui_mono_rain_icon[1][UI_MONO_RAIN_SIZE * UI_MONO_RAIN_SIZE / 8u];
