/*
 * Debug console (PLAN D57, CONFIG_EPAGER_DEBUG_CONSOLE, never in release builds): an esp_console REPL
 * on the USB Serial/JTAG port. Commands run in the REPL's task and only queue work; the main loop
 * applies it (app_console_poll), so the core keeps a single owner.
 *
 *   post TITLE [BODY] [CATEGORY]   a sample Android notification (category 0-11, default 4 social)
 *   remove | clear                 remove the last posted one | all
 *   time UTC_S [TZ_MIN]            set the clock (and time.tz_offset_min)
 *   pairing on|off
 *   unbond                         forget every bonded phone (pairing mode opens)
 *   brightness PCT | idle DIM_S OFF_S | orientation 0-3   display settings (as the app sets them)
 *   wifi [SSID [PASSWORD] | forget]   the network (as WIFI_SET from the app); no argument: Wi-Fi and weather state
 *   weather CITY... | ip | on | off | now   weather.city (ip: locate by IP), weather.enabled, fetch now
 *   setup on|off                   the Wi-Fi setup hotspot (as the pairing screen's button)
 *   batlog [clear]                 print the battery log | delete it (CONFIG_EPAGER_BATTERY_LOG)
 *   ab ...                         switches for A/B drain tests (app_ab.c, PLAN D106)
 *   tasks                          the busiest tasks since boot (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
 *   pm                             the power manager's lock and mode table (CONFIG_PM_PROFILING; over USB the chip never sleeps)
 * The password typed for `wifi` is echoed by the console: development only, like the rest.
 */
#include "app_priv.h"

#include "sdkconfig.h"

#if CONFIG_EPAGER_DEBUG_CONSOLE

#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "epager/proto_gen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TAG "console"

static QueueHandle_t q;

static int send(const app_console_cmd_t *c)
{
    return xQueueSend(q, c, pdMS_TO_TICKS(100)) == pdTRUE ? 0 : 1;
}

static int cmd_post(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: post TITLE [BODY] [CATEGORY]\n");
        return 1;
    }
    app_console_cmd_t c = {.kind = APP_CMD_POST, .category = 4};
    strlcpy(c.title, argv[1], sizeof(c.title));
    if (argc > 2) {
        strlcpy(c.body, argv[2], sizeof(c.body));
    }
    if (argc > 3) {
        c.category = (uint8_t)atoi(argv[3]);
    }
    return send(&c);
}

static int cmd_remove(int argc, char **argv)
{
    (void)argc;
    app_console_cmd_t c = {.kind = strcmp(argv[0], "clear") == 0 ? APP_CMD_CLEAR : APP_CMD_REMOVE};
    return send(&c);
}

static int cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: time UTC_S [TZ_MIN]\n");
        return 1;
    }
    app_console_cmd_t c = {.kind = APP_CMD_TIME, .utc = (uint32_t)strtoul(argv[1], NULL, 10), .has_tz = argc > 2};
    if (c.has_tz) {
        c.tz_min = (int16_t)atoi(argv[2]);
    }
    return send(&c);
}

static int cmd_pairing(int argc, char **argv)
{
    app_console_cmd_t c = {.kind = APP_CMD_PAIRING, .on = argc > 1 && strcmp(argv[1], "on") == 0};
    return send(&c);
}

static int cmd_unbond(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_console_cmd_t c = {.kind = APP_CMD_UNBOND};
    return send(&c);
}

static int config(uint16_t key, uint8_t size, const char *value)
{
    app_console_cmd_t c = {.kind = APP_CMD_CONFIG, .key = key, .size = size, .value = (uint16_t)strtoul(value, NULL, 10)};
    return send(&c);
}

static int cmd_display(int argc, char **argv)
{
    if (strcmp(argv[0], "idle") == 0 && argc == 3) {
        return config(PROTO_CFG_DISPLAY_IDLE_DIM_S, 2, argv[1]) | config(PROTO_CFG_DISPLAY_IDLE_OFF_S, 2, argv[2]);
    }
    if (argc != 2 || strcmp(argv[0], "idle") == 0) {
        printf("usage: brightness PCT | idle DIM_S OFF_S | orientation 0-3\n");
        return 1;
    }
    return config(strcmp(argv[0], "brightness") == 0 ? PROTO_CFG_DISPLAY_BRIGHTNESS : PROTO_CFG_DISPLAY_ORIENTATION, 1,
                  argv[1]);
}

static int cmd_wifi(int argc, char **argv)
{
    app_console_cmd_t c = {.kind = APP_CMD_STATUS};
    if (argc > 1 && strcmp(argv[1], "forget") == 0) {
        c.kind = APP_CMD_WIFI_FORGET;
    } else if (argc > 1) {
        if (strlen(argv[1]) > 32 || (argc > 2 && (strlen(argv[2]) < 8 || strlen(argv[2]) > 63))) {
            printf("usage: wifi SSID [PASSWORD] (name 1-32 bytes, password 8-63) | wifi forget | wifi\n");
            return 1;
        }
        c.kind = APP_CMD_WIFI;
        strlcpy(c.ssid, argv[1], sizeof(c.ssid));
        if (argc > 2) {
            strlcpy(c.pw, argv[2], sizeof(c.pw));
        }
    }
    const int r = send(&c);
    memset(c.pw, 0, sizeof(c.pw));
    return r;
}

static int cmd_weather(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: weather CITY... | ip | on | off | now\n");
        return 1;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        return config(PROTO_CFG_WEATHER_ENABLED, 1, strcmp(argv[1], "on") == 0 ? "1" : "0");
    }
    app_console_cmd_t c = {.kind = strcmp(argv[1], "now") == 0 ? APP_CMD_FETCH : APP_CMD_CITY};
    if (c.kind == APP_CMD_CITY && strcmp(argv[1], "ip") != 0) {
        for (int i = 1; i < argc; i++) { /* the words of the name, joined by spaces */
            if (i > 1) {
                strlcat(c.city, " ", sizeof(c.city));
            }
            strlcat(c.city, argv[i], sizeof(c.city));
        }
    }
    return send(&c);
}

static int cmd_setup(int argc, char **argv)
{
    app_console_cmd_t c = {.kind = APP_CMD_SETUP, .on = argc > 1 && strcmp(argv[1], "on") == 0};
    return send(&c);
}

static int cmd_batlog(int argc, char **argv)
{
    app_console_cmd_t c = {.kind = APP_CMD_BATLOG, .on = argc > 1 && strcmp(argv[1], "clear") == 0};
    return send(&c);
}

static int cmd_loop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_loop_dump();
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    app_tasks_dump(stdout, "");
#else
    printf("tasks: build with CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS (variants/sdkconfig.devconsole)\n");
#endif
    return 0;
}

static int cmd_pm(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);
#else
    printf("pm: build with CONFIG_PM_PROFILING (variants/sdkconfig.devconsole)\n");
#endif
    return 0;
}

static int cmd_ab(int argc, char **argv)
{
    app_console_cmd_t c = {.kind = APP_CMD_AB, .ab = AB_SHOW, .ab_n = {-1, -1, -1, 4000}};
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        c.ab = AB_RESET;
    } else if (argc >= 3 && strcmp(argv[1], "rail") == 0) {
        c.ab = AB_RAIL;
        c.ab_n[0] = strcmp(argv[2], "on") == 0;
    } else if (argc >= 3 && strcmp(argv[1], "ancs") == 0) {
        c.ab = AB_ANCS;
        c.ab_n[0] = strcmp(argv[2], "default") == 0 ? -1 : strcmp(argv[2], "off") == 0 ? 0 : atoi(argv[2]);
    } else if (argc >= 3 && strcmp(argv[1], "tick") == 0) {
        c.ab = AB_TICK;
        c.ab_n[0] = strcmp(argv[2], "default") == 0 ? -1 : atoi(argv[2]);
        if (c.ab_n[0] == 0 || (c.ab_n[0] > 0 && (c.ab_n[0] < 50 || c.ab_n[0] > 5000))) {
            printf("ab tick: default or 50-5000 ms\n");
            return 1;
        }
    } else if (argc >= 3 && strcmp(argv[1], "ble") == 0) {
        c.ab = AB_BLE;
        if (strcmp(argv[2], "default") != 0) {
            if (argc < 5) {
                printf("usage: ab ble default | ab ble MIN_MS MAX_MS LATENCY [TIMEOUT_MS]\n");
                return 1;
            }
            c.ab_n[0] = atoi(argv[2]);
            c.ab_n[1] = atoi(argv[3]);
            c.ab_n[2] = atoi(argv[4]);
            c.ab_n[3] = argc > 5 ? atoi(argv[5]) : 4000;
        }
    } else if (argc > 1) {
        printf("usage: ab | ab rail on|off | ab ble default|MIN_MS MAX_MS LATENCY [TIMEOUT_MS] | ab ancs default|off|SECONDS | ab tick default|MS | ab reset\n");
        return 1;
    }
    return send(&c);
}

void app_console_start(void)
{
    q = xQueueCreate(4, sizeof(app_console_cmd_t));
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "epager>";
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    const esp_console_cmd_t cmds[] = {
        {.command = "post", .help = "post TITLE [BODY] [CATEGORY]: a sample notification", .func = cmd_post},
        {.command = "remove", .help = "remove the last posted notification", .func = cmd_remove},
        {.command = "clear", .help = "remove all posted notifications", .func = cmd_remove},
        {.command = "time", .help = "time UTC_S [TZ_MIN]: set the clock", .func = cmd_time},
        {.command = "pairing", .help = "pairing on|off", .func = cmd_pairing},
        {.command = "unbond", .help = "forget every bonded phone", .func = cmd_unbond},
        {.command = "brightness", .help = "brightness PCT: display.brightness", .func = cmd_display},
        {.command = "idle", .help = "idle DIM_S OFF_S: display.idle_dim_s and idle_off_s (0 = never)", .func = cmd_display},
        {.command = "orientation", .help = "orientation 0-3: display.orientation", .func = cmd_display},
        {.command = "wifi", .help = "wifi [SSID [PASSWORD] | forget]: store the network; no argument: state",
         .func = cmd_wifi},
        {.command = "weather", .help = "weather CITY... | ip | on | off | now", .func = cmd_weather},
        {.command = "setup", .help = "setup on|off: the Wi-Fi setup hotspot", .func = cmd_setup},
        {.command = "batlog", .help = "batlog [clear]: print the battery log | delete it", .func = cmd_batlog},
        {.command = "loop", .help = "loop: the main loop passes and where their time goes, since the last 5-minute report", .func = cmd_loop},
        {.command = "tasks", .help = "tasks: the busiest tasks since boot (run-time stats)", .func = cmd_tasks},
        {.command = "pm", .help = "pm: the power manager's lock and mode table (CONFIG_PM_PROFILING)", .func = cmd_pm},
        {.command = "ab", .help = "ab [rail on|off | ble default|MIN MAX LATENCY [TIMEOUT] | ancs default|off|S | tick default|MS | reset]: A/B drain tests",
         .func = cmd_ab},
    };
    if (q == NULL || esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "debug console not started");
        return;
    }
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
    ESP_LOGW(TAG, "DEBUG console on: typed text becomes notification content");
}

void app_console_poll(void)
{
    app_console_cmd_t c;
    while (q != NULL && xQueueReceive(q, &c, 0) == pdTRUE) {
        app_console_apply(&c);
    }
}

#else

void app_console_start(void) {}
void app_console_poll(void) {}

#endif
