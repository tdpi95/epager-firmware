/* Inside components/app: the debug console's commands (PLAN D57) and their hand-over to the main loop;
 * the battery log (hw-checklist 27). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "epager/board_api.h"
#include "epager/kv_storage.h"
#include "nvs.h"

#define APP_ASSETS_MOUNT "/assets" /* LittleFS on the assets partition */

typedef enum {
    APP_CMD_POST, APP_CMD_REMOVE, APP_CMD_CLEAR, APP_CMD_TIME, APP_CMD_PAIRING, APP_CMD_CONFIG,
    APP_CMD_WIFI, APP_CMD_WIFI_FORGET, APP_CMD_STATUS, APP_CMD_CITY, APP_CMD_FETCH, APP_CMD_SETUP, APP_CMD_BATLOG, APP_CMD_UNBOND,
    APP_CMD_AB, APP_CMD_UPDATE, APP_CMD_FACE,
} app_cmd_kind_t;

/* APP_CMD_AB (app_ab.c): which A/B drain-test switch */
typedef enum { AB_SHOW, AB_RAIL, AB_BLE, AB_ANCS, AB_TICK, AB_RESET } app_ab_what_t;

typedef struct {
    app_cmd_kind_t kind;
    char title[96], body[192];
    uint8_t category;
    uint32_t utc;
    int16_t tz_min;
    bool has_tz, on;   /* APP_CMD_BATLOG: on = clear */
    uint16_t key;      /* APP_CMD_CONFIG: a u8 or u16 key, its wire size and value */
    uint8_t size;
    uint16_t value;
    char ssid[33], pw[64], city[65]; /* APP_CMD_WIFI, APP_CMD_CITY; APP_CMD_UPDATE, APP_CMD_FACE: title = the word, body = its argument, value = a number */
    app_ab_what_t ab;  /* APP_CMD_AB: the switch and its numbers (-1 = the default) */
    int32_t ab_n[4];
} app_console_cmd_t;

/* Start the REPL on the USB console (no-op without CONFIG_EPAGER_DEBUG_CONSOLE). */
void app_console_start(void);
/* Main loop: apply the commands typed since the last call (the REPL runs in its own task). */
void app_console_poll(void);
/* app.c: apply one command to the core (main loop task). */
void app_console_apply(const app_console_cmd_t *cmd);

/* Battery log (no-ops without CONFIG_EPAGER_BATTERY_LOG). Start after the assets mount: prints the
 * stored log when a USB host is attached and marks the boot. Tick from the main loop. */
void app_batlog_start(const board_profile_t *board, const char *firmware, bool fs_ok);
void app_batlog_tick(uint32_t now_ms, uint32_t utc);
/* What else a drain hunt needs next to the level (no-ops without the log): the phone link (0 down, 1 connected,
 * 2 secure), the Wi-Fi state (PROTO_WIFI_STATE_*; a change to CONNECTING counts one join) and each wake-up from
 * the light-sleep policy. The log writes the state and the counts since boot. */
void app_batlog_note_ble(uint8_t state);
void app_batlog_note_wifi(uint8_t state);
void app_batlog_note_wake(void);
/* One stretch with the sleep locks held ended (the policy armed again): its length in ms. The log writes the total
 * since boot and the longest stretch of each period. */
void app_batlog_note_awake(uint32_t ms);
/* A line "# TEXT" in the log (a test switch changed), so a night's numbers can be read with what was set. */
void app_batlog_note_text(const char *text);
/* The main loop statistics since the last "# loop" line, to the console (CONFIG_EPAGER_DEBUG_CONSOLE). */
void app_loop_dump(void);
void app_batlog_dump(void);
/* The busiest tasks and their run time since boot, one line each behind `prefix` (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS,
 * PLAN D108); the battery log and the console's `tasks` use it. */
#include <stdio.h>
void app_tasks_dump(FILE *out, const char *prefix);
void app_batlog_clear(void);

/* A/B drain-test switches (app_ab.c, PLAN D106; the debug console's `ab`): apply one, print the state and note it
 * in the battery log. RAM only: a reset restores the defaults. Main loop task. */
void app_ab_apply(const board_profile_t *board, const app_console_cmd_t *cmd);

/* Event ring in RTC memory for tests on battery (app_evlog.c, CONFIG_EPAGER_EVENT_LOG; no-ops otherwise): the boot log
 * prints the events of the earlier runs. */
typedef enum {
    APP_EV_BOOT = 1, APP_EV_SLEEP_ARM, APP_EV_SLEEP_DISARM, APP_EV_BLE_CONNECTED, APP_EV_BLE_SECURE,
    APP_EV_BLE_DISCONNECTED, APP_EV_RADIO_OFF, APP_EV_RADIO_ON,
} app_ev_t;
#if CONFIG_EPAGER_EVENT_LOG
void app_ev(uint16_t code, int32_t arg);
void app_ev_boot(void);
#else
static inline void app_ev(uint16_t code, int32_t arg) { (void)code; (void)arg; }
static inline void app_ev_boot(void) {}
#endif

/* kv_storage_t on one NVS namespace (app_kv.c); nvs_flash_init() first. */
typedef struct {
    kv_storage_t iface;
    nvs_handle_t h;
} kv_nvs_t;
bool app_kv_open(kv_nvs_t *k, const char *ns);
