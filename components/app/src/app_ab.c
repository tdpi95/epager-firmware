/*
 * Switches for A/B drain tests (PLAN D106, CONFIG_EPAGER_DEBUG_CONSOLE): the console's `ab` command changes one
 * suspect of the idle current at a time, the state goes into the battery log as a "# ab ..." line, and the log's
 * voltage-per-hour (and awake_ms) of the next hours says what it cost. RAM only: a reset restores the defaults.
 *
 *   ab                           the state
 *   ab rail on|off               the audio rail held on between I2C uses (default: switched around each use, D73)
 *   ab ble default               the idle (relaxed) connection parameters
 *   ab ble MIN_MS MAX_MS LATENCY [TIMEOUT_MS]   asked for in place of 90-120 ms, latency 4, 4000 ms; Apple's rules
 *                                apply (min a multiple of 15 ms, max >= min + 15, max x (latency + 1) <= 2 s,
 *                                x3 < timeout, timeout 2-6 s)
 *   ab ancs default|off|SECONDS  the longest wait between ANCS discovery retries while linked (default 120);
 *                                off = never retry
 *   ab tick default|MS           the BLE host task's tick (default 250 ms, 50-5000): the connection mode policy,
 *                                the advertising interval and the retries react up to one tick later
 *   ab reset                     all of them back to the defaults
 */
#include "app_priv.h"

#include "sdkconfig.h"

#include <stdio.h>

#include "epager/ancs_glue.h"
#include "epager/ble_core.h"

#if CONFIG_EPAGER_DEBUG_CONSOLE

static struct {
    bool rail;
    bool ble_set;
    bleu_conn_params_t ble;
    int32_t ancs_s; /* -1 default */
    int32_t tick_ms; /* -1 default */
} st = {.ancs_s = -1, .tick_ms = -1};

static void describe(char *out, size_t n)
{
    char ble[40], ancs[16], tick[16];
    if (st.ble_set) {
        snprintf(ble, sizeof(ble), "%u-%u ms/latency %u/%u ms", (unsigned)(st.ble.itvl_min * 5 / 4),
                 (unsigned)(st.ble.itvl_max * 5 / 4), (unsigned)st.ble.latency, (unsigned)(st.ble.timeout * 10));
    } else {
        snprintf(ble, sizeof(ble), "default");
    }
    if (st.ancs_s < 0) {
        snprintf(ancs, sizeof(ancs), "default");
    } else if (st.ancs_s == 0) {
        snprintf(ancs, sizeof(ancs), "off");
    } else {
        snprintf(ancs, sizeof(ancs), "%lds", (long)st.ancs_s);
    }
    if (st.tick_ms < 0) {
        snprintf(tick, sizeof(tick), "default");
    } else {
        snprintf(tick, sizeof(tick), "%ldms", (long)st.tick_ms);
    }
    snprintf(out, n, "ab rail=%s ble=%s ancs=%s tick=%s", st.rail ? "held" : "switched", ble, ancs, tick);
}

static void set_ble(const int32_t *n)
{
    if (n[0] < 0) {
        blec_set_idle_params(NULL);
        st.ble_set = false;
        return;
    }
    if (n[0] % 5 || n[1] % 5 || n[3] % 10) {
        printf("ab ble: milliseconds must be multiples of 5 (timeout 10)\n");
        return;
    }
    const bleu_conn_params_t p = {.itvl_min = (uint16_t)(n[0] * 4 / 5), .itvl_max = (uint16_t)(n[1] * 4 / 5),
                                  .latency = (uint16_t)n[2], .timeout = (uint16_t)(n[3] / 10)};
    if (!blec_set_idle_params(&p)) {
        printf("ab ble: refused, Apple's rules do not allow it (see app_ab.c)\n");
        return;
    }
    st.ble = p;
    st.ble_set = true;
}

void app_ab_apply(const board_profile_t *board, const app_console_cmd_t *cmd)
{
    const int32_t *n = cmd->ab_n;
    switch (cmd->ab) {
    case AB_SHOW:
        break;
    case AB_RAIL:
        if (board->set_rail == NULL) {
            printf("ab rail: this board has no switchable rail\n");
            return;
        }
        board->set_rail(board->ctx, BOARD_RAIL_AUDIO, n[0] != 0);
        st.rail = n[0] != 0;
        break;
    case AB_BLE:
        set_ble(n);
        break;
    case AB_ANCS:
        ancsg_set_retry_max_s(n[0] < 0 ? 120u : (uint32_t)n[0]);
        st.ancs_s = n[0];
        break;
    case AB_TICK:
        blec_set_tick_ms(n[0] < 0 ? 0u : (uint32_t)n[0]);
        st.tick_ms = n[0];
        break;
    case AB_RESET:
        if (board->set_rail != NULL) {
            board->set_rail(board->ctx, BOARD_RAIL_AUDIO, false);
        }
        st.rail = false;
        blec_set_idle_params(NULL);
        st.ble_set = false;
        ancsg_set_retry_max_s(120u);
        st.ancs_s = -1;
        blec_set_tick_ms(0);
        st.tick_ms = -1;
        break;
    }
    char line[112];
    describe(line, sizeof(line));
    printf("%s\n", line);
    if (cmd->ab != AB_SHOW) {
        app_batlog_note_text(line);
    }
}

#else

void app_ab_apply(const board_profile_t *board, const app_console_cmd_t *cmd)
{
    (void)board;
    (void)cmd;
}

#endif
