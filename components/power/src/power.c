#include "epager/power.h"

#include "sdkconfig.h"

#if CONFIG_PM_ENABLE

#include "esp_log.h"
#include "esp_pm.h"

#define TAG "power"

static esp_pm_lock_handle_t locks[POWER_LOCK_COUNT];

bool power_init(void)
{
    /* TODO(hw-verify): frequencies and idle current. There is no 32 kHz crystal on this board
     * (boards/waveshare_s3_epaper_154/src/ws154_pins.h), so the BLE low-power clock is the main XTAL
     * (sdkconfig.defaults) and light sleep keeps it powered (higher idle current than with a crystal). */
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#endif
    };
    if (esp_pm_configure(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed");
        return false;
    }
    return esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "epager_cpu", &locks[POWER_LOCK_CPU_MAX]) == ESP_OK &&
           esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epager_nosleep", &locks[POWER_LOCK_NO_SLEEP]) == ESP_OK;
}

void power_hold(power_lock_t lock)
{
    if (lock < POWER_LOCK_COUNT && locks[lock] != NULL) {
        esp_pm_lock_acquire(locks[lock]);
    }
}

void power_release(power_lock_t lock)
{
    if (lock < POWER_LOCK_COUNT && locks[lock] != NULL) {
        esp_pm_lock_release(locks[lock]);
    }
}

#else

bool power_init(void) { return true; }
void power_hold(power_lock_t lock) { (void)lock; }
void power_release(power_lock_t lock) { (void)lock; }

#endif
