/*
 * Battery log (CONFIG_EPAGER_BATTERY_LOG, hw-checklist 27): one CSV line per period in a file on the
 * assets partition, so readings taken on battery alone (no USB, no console) can be read back later.
 * At boot, while a USB host is attached, the whole log is printed between "batlog: begin" and
 * "batlog: end" (opening the monitor resets the board, so that is enough); the debug console's
 * `batlog` prints it again and `batlog clear` deletes it. Battery numbers only, no notification content.
 *
 *   # boot, reset R, firmware V        at every boot (R: esp_reset_reason_t, 9 = brown-out)
 *   # columns: up_s,utc,raw,...        right after it (the columns changed once: an old file stays readable)
 *   up_s,utc,raw,smoothed,pct,charging,ble,wifi,joins,wakes,drops
 *                                      one line per period (utc 0 = clock not set yet)
 *
 * awake_ms is the time since boot with both sleep locks held (the app was busy or the screen on: no light sleep) and
 * awake_max_ms the longest such stretch of the period; the chip also wakes for radio events while the locks are
 * released, which these do not count. A line "# ab ..." says which A/B test switches were set (app_ab.c).
 *
 * ble: 0 phone link down, 1 connected, 2 secure. wifi: PROTO_WIFI_STATE_*. joins, wakes and drops count since
 * boot: Wi-Fi join attempts, wake-ups from light sleep (the app's sleep policy) and phone links lost. The raw and
 * smoothed columns are in the board's own unit (ostb: ADC counts, paper: millivolts).
 *
 * Past CONFIG_EPAGER_BATTERY_LOG_MAX_KB the file becomes batlog.old (the previous .old is dropped).
 */
#include "app_priv.h"

#include "sdkconfig.h"

#if CONFIG_EPAGER_BATTERY_LOG

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#if CONFIG_PM_PROFILING
#include "esp_pm.h"
#endif
#if CONFIG_PM_PROFILING || CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include <stdlib.h>
#endif
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define TAG "batlog"
#define LOG_DIR APP_ASSETS_MOUNT "/diag" /* not among the assets (M6 lists the partition's root) */
#define LOG_CUR LOG_DIR "/batlog.csv"
#define LOG_OLD LOG_DIR "/batlog.old"
#define PERIOD_MS ((uint32_t)CONFIG_EPAGER_BATTERY_LOG_PERIOD_S * 1000u)
#define COLUMNS "up_s,utc,raw,smoothed,pct,charging,ble,wifi,joins,wakes,drops,awake_ms,awake_max_ms"
#define WIFI_CONNECTING 2 /* PROTO_WIFI_STATE_CONNECTING */

static const board_profile_t *board;
static bool on, write_failed;
static uint32_t last_ms;
static struct {
    uint8_t ble, wifi;
    uint32_t joins, wakes, drops;
    uint32_t awake_ms, awake_max_ms; /* sleep locks held: the total since boot, the longest stretch this period */
} note;

static void append(const char *line)
{
    FILE *f = fopen(LOG_CUR, "a");
    bool ok = f != NULL;
    if (f != NULL) {
        ok = fputs(line, f) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    if (!ok && !write_failed) {
        ESP_LOGW(TAG, "cannot write %s", LOG_CUR); /* once per failing stretch */
    }
    write_failed = !ok;
}

static void rotate(void)
{
    struct stat st;
    if (stat(LOG_CUR, &st) == 0 && st.st_size >= CONFIG_EPAGER_BATTERY_LOG_MAX_KB * 1024) {
        remove(LOG_OLD);
        rename(LOG_CUR, LOG_OLD);
    }
}

#if CONFIG_PM_PROFILING
/* PLAN D107: where the time goes in the power manager (the image of variants/sdkconfig.devconsole). Every period a
 * "# pm" line says how much of the time since boot the chip was in light sleep and how many sleeps were refused; every
 * sixth period the whole esp_pm_dump_locks table follows (which lock held the chip awake, for how long). */
static void note_pm(unsigned period)
{
    char *dump = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&dump, &size);
    if (f == NULL) {
        return;
    }
    esp_pm_dump_locks(f);
    fclose(f);
    if (dump == NULL) {
        return;
    }
    char *table = NULL; /* the whole dump, each line behind "# " */
    size_t table_size = 0;
    FILE *t = period % 6 == 0 ? open_memstream(&table, &table_size) : NULL;
    long long sleep_us = -1;
    int sleep_pct = -1;
    long ls = -1, rej = -1;
    for (char *line = dump; *line;) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
        }
        if (t != NULL && *line) {
            fprintf(t, "# %s\n", line);
        }
        if (strncmp(line, "SLEEP", 5) == 0) {
            sscanf(line, "SLEEP %*u M %lld %d %%", &sleep_us, &sleep_pct); /* "40 M", "0 %": the dump pads */
        } else if (strncmp(line, "light_sleep_counts:", 19) == 0) {
            sscanf(line, "light_sleep_counts:%ld light_sleep_reject_counts:%ld", &ls, &rej);
        }
        if (next == NULL) {
            break;
        }
        line = next + 1;
    }
    char out[96];
    snprintf(out, sizeof(out), "# pm sleep_us=%lld sleep_pct=%d ls=%ld rejected=%ld\n", sleep_us, sleep_pct, ls, rej);
    append(out);
    if (t != NULL) {
        fclose(t);
        if (table != NULL) {
            append(table);
        }
    }
    free(table);
    free(dump);
}
#endif

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* PLAN D108: which task uses the CPU. Run time since boot per task (the busiest first) and its share of one core's
 * time; the idle tasks include the time asleep. Every sixth period of the log, and the console's `tasks`. */
static int by_run_time(const void *a, const void *b)
{
    const TaskStatus_t *x = a, *y = b;
    return x->ulRunTimeCounter < y->ulRunTimeCounter ? 1 : x->ulRunTimeCounter > y->ulRunTimeCounter ? -1 : 0;
}

void app_tasks_dump(FILE *out, const char *prefix)
{
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = calloc(n, sizeof(*st));
    if (st == NULL) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    n = uxTaskGetSystemState(st, n, &total);
    qsort(st, n, sizeof(*st), by_run_time);
    fprintf(out, "%stasks: %-16s core  prio  run_us          one-core share\n", prefix, "name");
    for (UBaseType_t i = 0; i < n && i < 14; i++) {
        if (st[i].ulRunTimeCounter == 0) {
            break;
        }
        const int core = (int)xTaskGetCoreID(st[i].xHandle);
        fprintf(out, "%stasks: %-16s %-4s  %-4u  %-14lu  %5.1f%%\n", prefix, st[i].pcTaskName,
                core == 0 ? "0" : core == 1 ? "1" : "-", (unsigned)st[i].uxCurrentPriority,
                (unsigned long)st[i].ulRunTimeCounter, total ? 100.0 * st[i].ulRunTimeCounter / total : 0.0);
    }
    free(st);
}

static void note_tasks(void)
{
    char *text = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&text, &size);
    if (f == NULL) {
        return;
    }
    app_tasks_dump(f, "# ");
    fclose(f);
    if (text != NULL) {
        append(text);
        free(text);
    }
}
#endif

static void print_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char buf[96];
    while (fgets(buf, sizeof(buf), f) != NULL) {
        fputs(buf, stdout);
    }
    fclose(f);
}

void app_batlog_dump(void)
{
    if (!on) {
        printf("batlog: off\n");
        return;
    }
    printf("batlog: begin\n");
    print_file(LOG_OLD);
    print_file(LOG_CUR);
    printf("batlog: end\n");
    fflush(stdout);
}

void app_batlog_clear(void)
{
    remove(LOG_OLD);
    remove(LOG_CUR);
    ESP_LOGI(TAG, "cleared");
}

void app_batlog_start(const board_profile_t *b, const char *firmware, bool fs_ok)
{
    board = b;
    on = fs_ok && b->battery_diag != NULL;
    if (!on) {
        ESP_LOGW(TAG, "off: %s", fs_ok ? "the board gives no battery details" : "no file system");
        return;
    }
    mkdir(LOG_DIR, 0775); /* fails harmlessly when it exists */
    if (usb_serial_jtag_is_connected()) {
        app_batlog_dump();
    }
    rotate();
    char line[160];
    snprintf(line, sizeof(line), "# boot, reset %d, firmware %s\n# columns: " COLUMNS "\n", (int)esp_reset_reason(),
             firmware);
    append(line);
    ESP_LOGI(TAG, "on: a line every %d s in %s", CONFIG_EPAGER_BATTERY_LOG_PERIOD_S, LOG_CUR);
}

void app_batlog_tick(uint32_t now_ms, uint32_t utc)
{
    board_battery_diag_t d;
    if (!on || (last_ms != 0 && now_ms - last_ms < PERIOD_MS) || !board->battery_diag(board->ctx, &d)) {
        return; /* before the first reading: try again at the next tick */
    }
    last_ms = now_ms != 0 ? now_ms : 1;
    rotate();
    char line[144];
    snprintf(line, sizeof(line), "%lu,%lu,%ld,%ld,%u,%d,%u,%u,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)(now_ms / 1000u),
             (unsigned long)utc, (long)d.raw, (long)d.smoothed, d.pct, d.charging ? 1 : 0, note.ble, note.wifi,
             (unsigned long)note.joins, (unsigned long)note.wakes, (unsigned long)note.drops,
             (unsigned long)note.awake_ms, (unsigned long)note.awake_max_ms);
    note.awake_max_ms = 0;
    append(line);
#if CONFIG_PM_PROFILING
    static unsigned periods;
    note_pm(++periods);
#endif
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static unsigned task_periods;
    if (++task_periods % 6 == 0) {
        note_tasks();
    }
#endif
}

void app_batlog_note_ble(uint8_t state)
{
    if (state == 0 && note.ble != 0) {
        note.drops++;
    }
    note.ble = state;
}

void app_batlog_note_wifi(uint8_t state)
{
    if (state == WIFI_CONNECTING && note.wifi != WIFI_CONNECTING) {
        note.joins++;
    }
    note.wifi = state;
}

void app_batlog_note_wake(void)
{
    note.wakes++;
}

void app_batlog_note_awake(uint32_t ms)
{
    note.awake_ms += ms;
    if (ms > note.awake_max_ms) {
        note.awake_max_ms = ms;
    }
}

void app_batlog_note_text(const char *text)
{
    if (!on) {
        return;
    }
    char line[216];
    snprintf(line, sizeof(line), "# %s\n", text);
    append(line);
}

#else

void app_batlog_start(const board_profile_t *b, const char *firmware, bool fs_ok)
{
    (void)b;
    (void)firmware;
    (void)fs_ok;
}
void app_batlog_tick(uint32_t now_ms, uint32_t utc)
{
    (void)now_ms;
    (void)utc;
}
void app_batlog_dump(void) {}
void app_batlog_clear(void) {}
void app_batlog_note_ble(uint8_t state) { (void)state; }
void app_batlog_note_wifi(uint8_t state) { (void)state; }
void app_batlog_note_wake(void) {}
void app_batlog_note_awake(uint32_t ms) { (void)ms; }
void app_batlog_note_text(const char *text) { (void)text; }

#endif
