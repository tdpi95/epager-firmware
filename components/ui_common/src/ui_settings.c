/*
 * The settings list (PLAN D96, D97, D99): the rows, their values and what selecting one does. A choice goes to its
 * next value and is reported as UI_ACTION_SETTING (config key and value), which the firmware stores like a
 * CONFIG_SET, so the phone sees it too and the UI redraws from the new configuration. Three rows open a page:
 * the bonded phones and the saved Wi-Fi networks (selecting an entry asks "Forget?", a second select does it) and
 * About. Paper keys: short BOOT = up, short PWR = down, long BOOT = select, long PWR = back (ui_on_button).
 * Touch boards (the LCD, D99): a tap selects the row it hits, a swipe up or down turns the page of
 * UI_TOUCH_ROWS rows, a swipe right goes back (the colour layout draws it and calls ui_settings_tap/_scroll).
 */
#include <stdio.h>
#include <string.h>

#include "epager/proto_gen.h"
#include "ui_priv.h"

typedef enum {
    ROW_CLOCK,
    ROW_TEMP,
    ROW_LANGUAGE,
    ROW_BODY,
    ROW_MASK,
    ROW_ROTATION,
    ROW_WEATHER,
    ROW_PAIRING,
    ROW_WIFI,
    ROW_PHONES,
    ROW_NETWORKS,
    ROW_ABOUT,
} row_t;

typedef struct {
    row_t row;
    uint16_t key;     /* config key of a choice, 0 for an action or a page */
    uint8_t values;   /* how many values the choice cycles through */
    bool needs_wifi;
} item_t;

static const item_t ITEMS[] = {
    {ROW_CLOCK, PROTO_CFG_UI_CLOCK_24H, 2, false},
    {ROW_TEMP, PROTO_CFG_UI_TEMP_UNIT, 2, false},
    {ROW_LANGUAGE, PROTO_CFG_UI_LANGUAGE, 2, false},
    {ROW_BODY, PROTO_CFG_DISPLAY_SHOW_BODY, 2, false},
    {ROW_MASK, PROTO_CFG_DISPLAY_MASK_NUMBERS, 2, false},
    {ROW_ROTATION, PROTO_CFG_DISPLAY_ORIENTATION, 4, false},
    {ROW_WEATHER, PROTO_CFG_WEATHER_ENABLED, 2, true},
    {ROW_PAIRING, 0, 0, false},
    {ROW_WIFI, 0, 0, true},
    {ROW_PHONES, 0, 0, false},
    {ROW_NETWORKS, 0, 0, true},
    {ROW_ABOUT, 0, 0, false},
};

#define ABOUT_ROWS 5u

bool ui_keys_only(void) { return ui.p.display != NULL && ui.p.display->write_frame != NULL && ui.p.read_touch == NULL; }

static bool touch_list(void) { return ui.p.read_touch != NULL; }

size_t ui_settings_rows_shown(void) { return touch_list() ? UI_TOUCH_ROWS : UI_SETTINGS_ROWS; }

static const item_t *item_at(size_t i)
{
    for (size_t k = 0; k < sizeof(ITEMS) / sizeof(ITEMS[0]); k++) {
        if (ITEMS[k].needs_wifi && !ui.p.wifi_setup) {
            continue;
        }
        if (i-- == 0) {
            return &ITEMS[k];
        }
    }
    return NULL;
}

static size_t main_count(void)
{
    size_t n = 0;
    while (item_at(n)) {
        n++;
    }
    return n;
}

/* An empty list still has one row that says so. */
static size_t list_count(uint8_t n) { return n ? n : 1u; }

size_t ui_settings_count(void)
{
    switch (ui.set_page) {
    case UI_PAGE_PHONES: return list_count(ui.lists.n_phones);
    case UI_PAGE_NETWORKS: return list_count(ui.lists.n_networks);
    case UI_PAGE_ABOUT: return ABOUT_ROWS;
    default: return main_count();
    }
}

const char *ui_settings_title(void)
{
    const ui_strings_t *s = ui_strings();
    switch (ui.set_page) {
    case UI_PAGE_PHONES: return s->set_phones;
    case UI_PAGE_NETWORKS: return s->set_networks;
    case UI_PAGE_ABOUT: return s->set_about;
    default: return s->settings;
    }
}

static uint8_t current(const item_t *it)
{
    switch (it->row) {
    case ROW_CLOCK: return ui.cfg.clock_24h;
    case ROW_TEMP: return ui.cfg.temp_unit == PROTO_TEMP_UNIT_FAHRENHEIT;
    case ROW_LANGUAGE: return ui.cfg.language == PROTO_LANGUAGE_VI;
    case ROW_BODY: return ui.cfg.show_body;
    case ROW_MASK: return ui.cfg.mask_numbers;
    case ROW_ROTATION: /* the LCD driver turns the panel by 0 or 180 degrees only (the nearest of the four) */
        return touch_list() ? ((ui.cfg.orientation & 3u) >= 2u ? 2u : 0u) : (ui.cfg.orientation & 3u);
    case ROW_WEATHER: return ui.cfg.weather_enabled;
    default: return 0;
    }
}

static void main_row(size_t i, const char **name, char *value, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    const item_t *it = item_at(i);
    if (it == NULL) {
        return;
    }
    const uint8_t v = current(it);
    const char *text = "";
    char turn[8];
    switch (it->row) {
    case ROW_CLOCK: *name = s->set_clock; text = v ? "24h" : "12h"; break;
    case ROW_TEMP: *name = s->set_temp; text = v ? "°F" : "°C"; break;
    case ROW_LANGUAGE: *name = s->set_language; text = v ? "Tiếng Việt" : "English"; break;
    case ROW_BODY: *name = s->set_body; text = v ? s->on : s->off_value; break;
    case ROW_MASK: *name = s->set_mask; text = v ? s->on : s->off_value; break;
    case ROW_ROTATION:
        snprintf(turn, sizeof(turn), "%u°", v * 90u);
        *name = s->set_rotation;
        text = turn;
        break;
    case ROW_WEATHER: *name = s->set_weather; text = v ? s->on : s->off_value; break;
    case ROW_PAIRING: *name = s->set_pairing; break;
    case ROW_WIFI: *name = s->set_wifi; break;
    case ROW_PHONES: *name = s->set_phones; text = ">"; break;
    case ROW_NETWORKS: *name = s->set_networks; text = ">"; break;
    case ROW_ABOUT: *name = s->set_about; text = ">"; break;
    }
    snprintf(value, cap, "%s", text);
}

void ui_settings_row(size_t i, const char **name, char *value, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    static char label[48]; /* the one row being asked for; the layout copies it at once */
    *name = "";
    value[0] = '\0';
    switch (ui.set_page) {
    case UI_PAGE_PHONES:
        if (ui.lists.n_phones == 0) {
            *name = s->none_phones;
            break;
        }
        snprintf(label, sizeof(label), s->phone_n, (unsigned)i + 1u);
        *name = label;
        snprintf(value, cap, "%s", ui.set_confirm && i == ui.set_cur ? s->forget_ask
                                   : ui.lists.phone_connected[i]       ? s->phone_connected
                                                                      : "");
        break;
    case UI_PAGE_NETWORKS:
        if (ui.lists.n_networks == 0) {
            *name = s->none_networks;
            break;
        }
        snprintf(label, sizeof(label), "%s", ui.lists.networks[i]);
        *name = label;
        snprintf(value, cap, "%s", ui.set_confirm && i == ui.set_cur ? s->forget_ask : "");
        break;
    case UI_PAGE_ABOUT: {
        const char *names[ABOUT_ROWS] = {s->about_firmware, s->about_device, s->about_board, s->about_battery,
                                         s->about_protocol};
        *name = names[i < ABOUT_ROWS ? i : 0];
        switch (i) {
        case 0: snprintf(value, cap, "%s", ui.lists.firmware); break;
        case 1: snprintf(value, cap, "%s", ui.lists.device); break;
        case 2: { /* "vendor_model": the vendor part leaves no room beside the label */
            const char *model = strchr(ui.lists.board, '_');
            snprintf(value, cap, "%s", model ? model + 1 : ui.lists.board);
            break;
        }
        case 3:
            if (ui.st.battery_pct <= 100) {
                snprintf(value, cap, "%u%%", ui.st.battery_pct);
            } else {
                snprintf(value, cap, "--");
            }
            break;
        default: snprintf(value, cap, "%s", PROTO_CONTRACT_VERSION); break;
        }
        break;
    }
    default:
        main_row(i, name, value, cap);
        break;
    }
}

/* Keys: keep the highlighted row inside the rows on screen. Touch: there is no highlight, only the page, which
 * must not start past the end of the list (the last page may be short). */
static void fix_window(void)
{
    const size_t rows = ui_settings_rows_shown();
    if (touch_list()) {
        const size_t n = ui_settings_count();
        if (ui.set_top >= n) {
            ui.set_top = n ? (n - 1) / rows * rows : 0;
        }
    } else if (ui.set_cur < ui.set_top) {
        ui.set_top = ui.set_cur;
    } else if (ui.set_cur >= ui.set_top + rows) {
        ui.set_top = ui.set_cur + 1 - rows;
    }
}

void ui_settings_clamp(void)
{
    const size_t n = ui_settings_count();
    if (ui.set_cur >= n) {
        ui.set_cur = n ? n - 1 : 0;
        ui.set_confirm = false;
    }
    fix_window();
}

void ui_settings_enter(void)
{
    ui.settings = true;
    ui.set_page = UI_PAGE_MAIN;
    ui.set_cur = ui.set_top = 0;
    ui.set_confirm = false;
    ui.model_dirty = true;
}

void ui_settings_back(void)
{
    ui.set_confirm = false;
    ui.model_dirty = true;
    if (ui.set_page == UI_PAGE_MAIN) {
        ui.settings = false;
        return;
    }
    ui.set_page = UI_PAGE_MAIN;
    ui.set_cur = ui.set_main_cur;
    ui.set_top = touch_list() ? ui.set_cur - ui.set_cur % UI_TOUCH_ROWS : 0; /* the page the row is on */
    fix_window();
}

void ui_settings_step(int dir)
{
    const size_t n = ui_settings_count();
    if (n == 0) {
        return;
    }
    ui.set_confirm = false;
    ui.set_cur = dir > 0 ? (ui.set_cur + 1) % n : (ui.set_cur + n - 1) % n;
    fix_window();
    ui.model_dirty = true;
}

void ui_settings_scroll(int dir)
{
    const size_t rows = ui_settings_rows_shown();
    const size_t top = ui.set_top;
    if (dir > 0 ? top + rows < ui_settings_count() : top > 0) {
        ui.set_top = dir > 0 ? top + rows : top - rows;
        ui.set_confirm = false;
        ui.model_dirty = true;
    }
}

static void open_page(uint8_t page)
{
    ui.set_main_cur = ui.set_cur;
    ui.set_page = page;
    ui.set_cur = ui.set_top = 0;
    ui.set_confirm = false;
    ui.model_dirty = true;
    ui_emit(UI_ACTION_INFO_REFRESH, 0); /* the firmware hands over the lists, now */
}

ui_action_t ui_settings_select(void)
{
    if (ui.set_page == UI_PAGE_PHONES || ui.set_page == UI_PAGE_NETWORKS) {
        const bool phones = ui.set_page == UI_PAGE_PHONES;
        if (ui.set_cur >= (phones ? ui.lists.n_phones : ui.lists.n_networks)) {
            return UI_ACTION_NONE; /* the "none" row */
        }
        ui.model_dirty = true;
        if (!ui.set_confirm) {
            ui.set_confirm = true;
            return UI_ACTION_NONE;
        }
        ui.set_confirm = false;
        ui_emit(phones ? UI_ACTION_FORGET_PHONE : UI_ACTION_FORGET_NETWORK, (uint32_t)ui.set_cur);
        return UI_ACTION_NONE;
    }
    if (ui.set_page == UI_PAGE_ABOUT) {
        return UI_ACTION_NONE;
    }
    const item_t *it = item_at(ui.set_cur);
    if (it == NULL) {
        return UI_ACTION_NONE;
    }
    if (it->key) {
        const uint32_t step = it->row == ROW_ROTATION && touch_list() ? 2u : 1u;
        const uint32_t next = (current(it) + step) % it->values;
        ui_emit(UI_ACTION_SETTING, (uint32_t)it->key << 8 | next);
        return UI_ACTION_NONE;
    }
    switch (it->row) {
    case ROW_PAIRING:
        ui.settings = false; /* pairing is a screen of its own: back to the clock when it ends */
        ui.model_dirty = true;
        return UI_ACTION_PAIRING_TOGGLE;
    case ROW_PHONES: open_page(UI_PAGE_PHONES); return UI_ACTION_NONE;
    case ROW_NETWORKS: open_page(UI_PAGE_NETWORKS); return UI_ACTION_NONE;
    case ROW_ABOUT: open_page(UI_PAGE_ABOUT); return UI_ACTION_NONE;
    default: return UI_ACTION_WIFI_SETUP; /* the list stays under the setup screen */
    }
}

void ui_settings_tap(size_t row)
{
    if (row >= ui_settings_count()) {
        return;
    }
    if (row != ui.set_cur) {
        ui.set_confirm = false; /* "Forget?" belongs to the row it was asked on */
    }
    ui.set_cur = row;
    ui.model_dirty = true;
    const ui_action_t a = ui_settings_select();
    if (a != UI_ACTION_NONE) {
        ui_emit(a, 0);
    }
}

void ui_set_info(const ui_info_t *info)
{
    if (memcmp(info, &ui.lists, sizeof(*info)) == 0) {
        return;
    }
    ui.lists = *info;
    ui_settings_clamp();
    ui.model_dirty = true;
}
