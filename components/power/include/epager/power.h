/*
 * Power management skeleton (PLAN D17): never deep sleep; DFS plus automatic light sleep so the BLE
 * link stays up, and PM locks held by the work that must not be slowed down or interrupted. Without
 * CONFIG_PM_ENABLE every call is a no-op. ESP-IDF glue (not portable).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_LOCK_CPU_MAX = 0,   /* full CPU clock: rendering, hashing, flash writes during a transfer */
    POWER_LOCK_NO_SLEEP = 1,  /* no light sleep: a panel refresh or an SPI burst in progress */
    POWER_LOCK_COUNT
} power_lock_t;

/* Configure DFS and automatic light sleep. False if the configuration was refused. */
bool power_init(void);
/* Locks are counted: every hold needs one release. */
void power_hold(power_lock_t lock);
void power_release(power_lock_t lock);

#ifdef __cplusplus
}
#endif
