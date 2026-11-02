/*
 * Event ring (CONFIG_EPAGER_EVENT_LOG, PLAN D86): the last events in RTC memory, which survives a reset but
 * not a power loss, so a test on battery can be read after the next USB connection: the boot log prints what the
 * earlier runs recorded. No notification content, only kinds, reasons and times (privacy rules).
 */
#include "app_priv.h"

#include "sdkconfig.h"

#if CONFIG_EPAGER_EVENT_LOG

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"

#define EV_N 96
#define EV_MAGIC 0x45564C47u

static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint16_t boots, head, count;
    struct {
        uint32_t ms;
        uint16_t boot, code;
        int32_t arg;
    } e[EV_N];
} ring;

static const char *name(uint16_t code)
{
    switch (code) {
    case APP_EV_BOOT: return "boot (reset reason)";
    case APP_EV_SLEEP_ARM: return "light sleep allowed";
    case APP_EV_SLEEP_DISARM: return "light sleep off";
    case APP_EV_BLE_CONNECTED: return "phone connected";
    case APP_EV_BLE_SECURE: return "link secure";
    case APP_EV_BLE_DISCONNECTED: return "phone disconnected (reason)";
    default: return "?";
    }
}

void app_ev(uint16_t code, int32_t arg)
{
    ring.e[ring.head].ms = (uint32_t)(esp_timer_get_time() / 1000);
    ring.e[ring.head].boot = ring.boots;
    ring.e[ring.head].code = code;
    ring.e[ring.head].arg = arg;
    ring.head = (uint16_t)((ring.head + 1) % EV_N);
    if (ring.count < EV_N) {
        ring.count++;
    }
}

void app_ev_boot(void)
{
    if (ring.magic != EV_MAGIC) {
        memset(&ring, 0, sizeof(ring));
        ring.magic = EV_MAGIC;
    } else if (ring.count) {
        printf("events of the earlier runs (boot, ms since that boot, event, value):\n");
        for (uint16_t i = 0; i < ring.count; i++) {
            const uint16_t k = (uint16_t)((ring.head + EV_N - ring.count + i) % EV_N);
            printf("  #%u %8lu ms  %s %ld\n", (unsigned)ring.e[k].boot, (unsigned long)ring.e[k].ms, name(ring.e[k].code),
                   (long)ring.e[k].arg);
        }
        printf("end of events\n");
    }
    ring.boots++;
    app_ev(APP_EV_BOOT, (int32_t)esp_reset_reason()); /* 1 power on, 3 software, 4 panic, 5-7 watchdog, 9 brownout, 11 USB */
}

#endif
