/*
 * The settings list (PLAN D96, D97, D99): the rows, their values and what selecting one does. A choice goes to its
 * next value and is reported as UI_ACTION_SETTING (config key and value), which the firmware stores like a
 * CONFIG_SET, so the phone sees it too and the UI redraws from the new configuration. Three rows open a page:
 * the bonded phones and the saved Wi-Fi networks (selecting an entry asks "Forget?", a second select does it) and
 * About, and (boards with Wi-Fi updates) the Software update page: a status row and the actions that fit the state
 * (Install, Check now, Remind later, Skip this version, Cancel). The firmware opens that page by itself as the
 * update prompt (ui_show_update_prompt). Paper keys: short BOOT = up, short PWR = down, long BOOT = select, long PWR = back (ui_on_button).
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
    ROW_AUTOCHECK,
    ROW_PAIRING,
    ROW_WIFI,
    ROW_PHONES,
    ROW_NETWORKS,
    ROW_UPDATE,
    ROW_FACE,
    ROW_ABOUT,
} row_t;

typedef struct {
    row_t row;
    uint16_t key;     /* config key of a choice, 0 for an action or a page */
    uint8_t values;   /* how many values the choice cycles through */
    bool needs_wifi;
    bool needs_update; /* only on boards with Wi-Fi updates (ui_params_t.update) */
} item_t;

static const item_t ITEMS[] = {
    {ROW_CLOCK, PROTO_CFG_UI_CLOCK_24H, 2, false, false},
    {ROW_TEMP, PROTO_CFG_UI_TEMP_UNIT, 2, false, false},
    {ROW_LANGUAGE, PROTO_CFG_UI_LANGUAGE, 2, false, false},
    {ROW_BODY, PROTO_CFG_DISPLAY_SHOW_BODY, 2, false, false},
    {ROW_MASK, PROTO_CFG_DISPLAY_MASK_NUMBERS, 2, false, false},
    {ROW_ROTATION, PROTO_CFG_DISPLAY_ORIENTATION, 4, false, false},
    {ROW_WEATHER, PROTO_CFG_WEATHER_ENABLED, 2, true, false},
    {ROW_AUTOCHECK, PROTO_CFG_UPDATE_AUTO_CHECK, 2, true, true},
    {ROW_PAIRING, 0, 0, false, false},
    {ROW_WIFI, 0, 0, true, false},
    {ROW_PHONES, 0, 0, false, false},
    {ROW_NETWORKS, 0, 0, true, false},
    {ROW_UPDATE, 0, 0, true, true},
    {ROW_FACE, 0, 0, false, false}, /* before About, so the rows above keep their places */
    {ROW_ABOUT, 0, 0, false, false},
};

#define ABOUT_ROWS 5u

bool ui_keys_only(void) { return ui.p.display != NULL && ui.p.display->write_frame != NULL && ui.p.read_touch == NULL; }

static bool touch_list(void) { return ui.p.read_touch != NULL; }

size_t ui_settings_rows_shown(void) { return touch_list() ? UI_TOUCH_ROWS : UI_SETTINGS_ROWS; }

static const item_t *item_at(size_t i)
{
    for (size_t k = 0; k < sizeof(ITEMS) / sizeof(ITEMS[0]); k++) {
        if ((ITEMS[k].needs_wifi && !ui.p.wifi_setup) || (ITEMS[k].needs_update && !ui.p.update) ||
            (ITEMS[k].row == ROW_FACE && ui.p.no_faces)) {
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

/* ---------------------------------------------------------------- the Software update page */

typedef enum { UR_STATUS, UR_SIZE, UR_INSTALL, UR_CHECK, UR_LATER, UR_SKIP, UR_CANCEL } upd_row_t;
#define UPD_ROWS_MAX 7u

/* The rows for the state: a status row, then what the person can do now. */
static size_t upd_rows(upd_row_t out[UPD_ROWS_MAX])
{
    size_t n = 0;
    out[n++] = UR_STATUS;
    switch (ui.upd.state) {
    case UI_UPD_AVAILABLE:
        if (ui.upd.size && !ui.upd_prompt) { /* a prompt is four rows: the whole question on one page of the LCD */
            out[n++] = UR_SIZE;
        }
        out[n++] = UR_INSTALL;
        out[n++] = UR_LATER;
        if (!ui.upd.skipped) {
            out[n++] = UR_SKIP;
        }
        if (!ui.upd_prompt) {
            out[n++] = UR_CHECK;
        }
        break;
    case UI_UPD_DOWNLOADING:
        out[n++] = UR_CANCEL;
        break;
    case UI_UPD_CHECKING:
        break;
    case UI_UPD_FAILED:
        if (ui.upd.version[0]) {
            out[n++] = UR_INSTALL; /* try again */
        }
        out[n++] = UR_CHECK;
        break;
    default:
        out[n++] = UR_CHECK;
        break;
    }
    return n;
}

static size_t upd_count(void)
{
    upd_row_t rows[UPD_ROWS_MAX];
    return upd_rows(rows);
}

static void upd_row(size_t i, const char **name, char *value, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    upd_row_t rows[UPD_ROWS_MAX];
    const size_t n = upd_rows(rows);
    switch (i < n ? rows[i] : UR_STATUS) {
    case UR_STATUS:
        switch (ui.upd.state) {
        case UI_UPD_CHECKING: *name = s->upd_checking; break;
        case UI_UPD_UP_TO_DATE: *name = s->upd_current; break;
        case UI_UPD_AVAILABLE:
            *name = ui.upd_prompt ? s->upd_mark : ui.upd.skipped ? s->upd_skipped : s->upd_available;
            snprintf(value, cap, "%s", ui.upd.version);
            break;
        case UI_UPD_DOWNLOADING:
            *name = s->upd_updating;
            if (ui.st.ota_pct >= 0) {
                snprintf(value, cap, "%d%%", ui.st.ota_pct);
            }
            break;
        case UI_UPD_FAILED: *name = s->upd_err[ui.upd.error < UI_UPD_ERR_COUNT ? ui.upd.error : UI_UPD_ERR_INTERNAL]; break;
        case UI_UPD_NEEDS_APP:
            *name = s->upd_needs_app;
            snprintf(value, cap, "%s", ui.upd.version);
            break;
        default: *name = s->upd_none; break;
        }
        break;
    case UR_SIZE:
        *name = s->upd_size;
        snprintf(value, cap, "%lu.%lu MB", (unsigned long)(ui.upd.size / 1000000u), (unsigned long)(ui.upd.size / 100000u % 10u));
        break;
    case UR_INSTALL: *name = s->upd_install; break;
    case UR_CHECK: *name = s->upd_check; break;
    case UR_LATER: *name = s->upd_later; break;
    case UR_SKIP: *name = s->upd_skip; break;
    case UR_CANCEL: *name = s->upd_cancel; break;
    }
}

/* Selecting a row of the page. Remind later and Skip end a prompt: back to the clock. */
static ui_action_t upd_select(void)
{
    upd_row_t rows[UPD_ROWS_MAX];
    const size_t n = upd_rows(rows);
    if (ui.set_cur >= n) {
        return UI_ACTION_NONE;
    }
    ui_action_t a = UI_ACTION_NONE;
    switch (rows[ui.set_cur]) {
    case UR_INSTALL: a = UI_ACTION_UPDATE_INSTALL; ui.upd_prompt = false; break;
    case UR_CHECK: a = UI_ACTION_UPDATE_CHECK; break;
    case UR_CANCEL: a = UI_ACTION_UPDATE_CANCEL; break;
    case UR_LATER: a = UI_ACTION_UPDATE_LATER; break;
    case UR_SKIP: a = UI_ACTION_UPDATE_SKIP; break;
    default: break; /* the status and size rows only say something */
    }
    if (a == UI_ACTION_UPDATE_LATER || a == UI_ACTION_UPDATE_SKIP) {
        ui_emit(a, 0);
        ui_settings_back(); /* a prompt closes to the clock, the page to the list */
        return UI_ACTION_NONE;
    }
    if (a != UI_ACTION_NONE) {
        ui.set_cur = 0; /* the rows change with the state the firmware reports next */
        ui.set_top = 0;
        ui.model_dirty = true;
        ui_emit(a, 0);
    }
    return UI_ACTION_NONE;
}

/* ---------------------------------------------------------------- the Clock face pages (M8b) */

/* The list: Classic, the built-in faces, the installed ones. Selecting a face that is not in use uses it at once; selecting
 * the one in use opens its page (its name is the title) when there is something to do there: the face's options (a
 * select moves one to its next value) and, for an installed face, Delete (asks first, like Forget). */
typedef enum { FR_OPTION, FR_DELETE } face_row_t;
#define FACE_ROWS_MAX (1u + 8u)

static bool face_is_active(size_t i)
{
    const char *want = ui.cfg.clock_face_id;
    for (size_t k = 0; k < ui.n_faces; k++) {
        if (strcmp(ui.faces[k].id, want) == 0) {
            return k == i;
        }
    }
    return i == 0; /* an id that is not installed shows Classic */
}

static const char *face_name(const ui_face_entry_t *e)
{
    const bool vi = ui.cfg.language == PROTO_LANGUAGE_VI;
    if (e->id[0] == '\0') {
        return ui_strings()->face_classic;
    }
    return vi && e->name[1][0] ? e->name[1] : e->name[0][0] ? e->name[0] : e->id;
}

static void faces_refresh(void)
{
    ui.n_faces = ui_faces_collect(ui.faces, UI_FACES_MAX);
}

/* The rows of the page of face `face_sel`: option rows only while it is the face in use. */
static size_t face_rows(face_row_t out[FACE_ROWS_MAX])
{
    size_t n = 0;
    if (face_is_active(ui.face_sel)) {
        for (size_t i = 0; i < ui_face_option_count() && n < FACE_ROWS_MAX - 1u; i++) {
            out[n++] = FR_OPTION;
        }
    }
    if (!ui.faces[ui.face_sel].builtin) {
        out[n++] = FR_DELETE;
    }
    return n;
}

static void faces_row(size_t i, const char **name, char *value, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    if (i >= ui.n_faces) {
        return;
    }
    *name = face_name(&ui.faces[i]);
    snprintf(value, cap, "%s", face_is_active(i) ? s->face_active : ui.faces[i].local ? s->face_file : "");
}

static void face_row(size_t i, const char **name, char *value, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    face_row_t rows[FACE_ROWS_MAX];
    const size_t n = face_rows(rows);
    if (i >= n) {
        return;
    }
    switch (rows[i]) {
    case FR_OPTION: {
        uint8_t id, next;
        size_t k = 0; /* the option row's number among the option rows */
        for (size_t j = 0; j < i; j++) {
            k += rows[j] == FR_OPTION;
        }
        (void)ui_face_option_row(k, name, value, cap, &id, &next);
        break;
    }
    case FR_DELETE:
        *name = s->face_delete;
        snprintf(value, cap, "%s", ui.set_confirm && i == ui.set_cur ? s->face_delete_ask : "");
        break;
    }
}

static ui_action_t face_select(void)
{
    face_row_t rows[FACE_ROWS_MAX];
    const size_t n = face_rows(rows);
    if (ui.set_cur >= n) {
        return UI_ACTION_NONE;
    }
    switch (rows[ui.set_cur]) {
    case FR_OPTION: {
        const char *label;
        char value[2];
        uint8_t id, next;
        size_t k = 0;
        for (size_t j = 0; j < ui.set_cur; j++) {
            k += rows[j] == FR_OPTION;
        }
        if (ui_face_option_row(k, &label, value, sizeof(value), &id, &next)) {
            ui_emit(UI_ACTION_FACE_OPTION, (uint32_t)id << 8 | next);
        }
        break;
    }
    case FR_DELETE:
        ui.model_dirty = true;
        if (!ui.set_confirm) {
            ui.set_confirm = true;
        } else {
            ui.set_confirm = false;
            ui_emit(UI_ACTION_FACE_DELETE, (uint32_t)ui.face_sel);
            ui_settings_back();
        }
        break;
    default: break;
    }
    return UI_ACTION_NONE;
}

/* The configuration changed: a face's page closes when its face is no longer the one in use (a phone chose another). */
void ui_settings_config_changed(void)
{
    if (ui.settings && ui.set_page == UI_PAGE_FACE && !face_is_active(ui.face_sel)) {
        ui_settings_faces_changed(); /* reads the list again and goes back to it */
    }
}

const char *ui_face_list_id(size_t index) { return index < ui.n_faces ? ui.faces[index].id : ""; }

/* The installed faces changed (a phone installed or deleted one): the list is read again; a face's page closes, its index
 * may mean another face now. */
void ui_settings_faces_changed(void)
{
    if (!ui.settings || (ui.set_page != UI_PAGE_FACES && ui.set_page != UI_PAGE_FACE)) {
        return;
    }
    faces_refresh();
    if (ui.set_page == UI_PAGE_FACE) {
        ui.set_page = UI_PAGE_FACES;
        ui.set_confirm = false;
        ui.set_cur = ui.set_faces_cur;
    }
    ui_settings_clamp();
    ui.model_dirty = true;
}

/* An empty list still has one row that says so. */
static size_t list_count(uint8_t n) { return n ? n : 1u; }

size_t ui_settings_count(void)
{
    switch (ui.set_page) {
    case UI_PAGE_PHONES: return list_count(ui.lists.n_phones);
    case UI_PAGE_NETWORKS: return list_count(ui.lists.n_networks);
    case UI_PAGE_ABOUT: return ABOUT_ROWS;
    case UI_PAGE_UPDATE: return upd_count();
    case UI_PAGE_FACES: return ui.n_faces;
    case UI_PAGE_FACE: {
        face_row_t rows[FACE_ROWS_MAX];
        return face_rows(rows);
    }
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
    case UI_PAGE_UPDATE: return s->set_update;
    case UI_PAGE_FACES: return s->set_face;
    case UI_PAGE_FACE: return face_name(&ui.faces[ui.face_sel < ui.n_faces ? ui.face_sel : 0]);
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
    case ROW_AUTOCHECK: return ui.cfg.update_auto_check;
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
    case ROW_AUTOCHECK: *name = s->set_autocheck; text = v ? s->on : s->off_value; break;
    case ROW_PAIRING: *name = s->set_pairing; break;
    case ROW_WIFI: *name = s->set_wifi; break;
    case ROW_PHONES: *name = s->set_phones; text = ">"; break;
    case ROW_NETWORKS: *name = s->set_networks; text = ">"; break;
    case ROW_UPDATE: *name = s->set_update; text = ui.upd.mark ? ui.upd.version : ">"; break; /* the version says there is one */
    case ROW_ABOUT: *name = s->set_about; text = ">"; break;
    case ROW_FACE: *name = s->set_face; text = ">"; break;
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
    case UI_PAGE_UPDATE:
        upd_row(i, name, value, cap);
        break;
    case UI_PAGE_FACES:
        faces_row(i, name, value, cap);
        break;
    case UI_PAGE_FACE:
        face_row(i, name, value, cap);
        break;
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
    ui.upd_prompt = false;
    ui.model_dirty = true;
}

void ui_settings_back(void)
{
    ui.set_confirm = false;
    ui.model_dirty = true;
    if (ui.set_page == UI_PAGE_MAIN || (ui.set_page == UI_PAGE_UPDATE && ui.upd_prompt)) {
        ui.settings = false; /* the list, or the update prompt that the firmware opened: back to the clock */
        ui.upd_prompt = false;
        return;
    }
    if (ui.set_page == UI_PAGE_FACE) { /* a face's page: back to the list of faces, on its row */
        faces_refresh();
        ui.set_page = UI_PAGE_FACES;
        ui.set_cur = ui.set_faces_cur < ui.n_faces ? ui.set_faces_cur : 0;
    } else {
        ui.set_page = UI_PAGE_MAIN;
        ui.set_cur = ui.set_main_cur;
    }
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
    if (ui.set_page == UI_PAGE_UPDATE) {
        return upd_select();
    }
    if (ui.set_page == UI_PAGE_FACES) {
        if (ui.set_cur >= ui.n_faces) {
            return UI_ACTION_NONE;
        }
        if (!face_is_active(ui.set_cur)) { /* use it: the firmware stores the choice and the list moves "In use" */
            ui_emit(UI_ACTION_FACE_SELECT, (uint32_t)ui.set_cur);
        } else if (ui_face_option_count() > 0 || !ui.faces[ui.set_cur].builtin) {
            ui.set_faces_cur = ui.set_cur;
            ui.face_sel = ui.set_cur;
            ui.set_page = UI_PAGE_FACE;
            ui.set_cur = ui.set_top = 0;
            ui.set_confirm = false;
            ui.model_dirty = true;
        }
        return UI_ACTION_NONE;
    }
    if (ui.set_page == UI_PAGE_FACE) {
        return face_select();
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
    case ROW_FACE:
        ui.set_main_cur = ui.set_cur;
        faces_refresh();
        ui.set_page = UI_PAGE_FACES;
        ui.set_cur = ui.set_top = 0;
        ui.set_confirm = false;
        ui.model_dirty = true;
        return UI_ACTION_NONE;
    case ROW_UPDATE:
        ui.upd_prompt = false;
        open_page(UI_PAGE_UPDATE);
        return UI_ACTION_NONE;
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
