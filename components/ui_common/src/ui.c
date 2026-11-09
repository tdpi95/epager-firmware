/*
 * ui_common core (PLAN D51): the public API of epager/ui.h, the model (cards, status, time, climate,
 * configuration), strings and formatting shared by the layouts, and the LVGL glue. The display backend
 * and the layout are chosen from the board's display at ui_init.
 */
#include "epager/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "epager/board_util.h"
#include "epager/proto_gen.h"
#include "ui_icons.h"
#include "ui_priv.h"

UI_EXT_BSS ui_state_t ui;

static const ui_strings_t STRINGS[] = {
    [PROTO_LANGUAGE_EN] = {"Time not set", "Pairing mode", "Powered off", "Updating %d%%", "Temp", "Humidity",
                           {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
                           {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
                           false,
                           "Open the ePager app, or Bluetooth settings on your iPhone, to connect.", "Wi-Fi setup",
                           "Close", "Join Wi-Fi\n%s\nPassword\n%s\nthen open\n%s",
                           "Connecting to\n%s ...", "Connected to\n%s", "Could not connect to\n%s\nCheck the password.",
                           "Settings", "Enter this code on your phone", "Pairing code",
                           "BOOT: Wi-Fi setup", "Hold BOOT+PWR: cancel", "Join %s\nPassword %s\nthen open %s",
                           "Any key: close", "Setting updated", "Wi-Fi saved",
                           "Clock", "Temperature", "Language", "Message text", "Hide numbers", "Rotation", "Weather",
                           "Pairing mode", "Wi-Fi setup", "On", "Off",
                           "Paired phones", "Wi-Fi networks", "About", "Forget?", "No phones", "No networks", "Phone %u",
                           "connected", "Firmware", "Device", "Board", "Battery", "Protocol",
                           "Check updates", "Software update", "Update available",
                           "Not checked", "Checking...", "Up to date", "New version", "Skipped", "Updating", "Needs newer app", "Size",
                           "Install", "Check now", "Remind later", "Skip this version", "Cancel",
                           {"", "No Wi-Fi saved", "No connection", "Clock not set", "Download failed", "Update rejected",
                            "Phone update running", "Cancelled", "Battery below 30%", "Update failed"},
                           "Clock face", "Classic", "In use", "From file", "Delete", "Delete?"},
    [PROTO_LANGUAGE_VI] = {"Chưa đặt giờ", "Chế độ ghép nối", "Đã tắt nguồn", "Đang cập nhật %d%%", "Nhiệt độ", "Độ ẩm",
                           {"Chủ nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"},
                           {0},
                           true,
                           "Mở ứng dụng ePager, hoặc cài đặt Bluetooth trên iPhone, để kết nối.", "Cài đặt Wi-Fi",
                           "Đóng", "Kết nối Wi-Fi\n%s\nMật khẩu\n%s\nrồi mở\n%s",
                           "Đang kết nối\n%s ...", "Đã kết nối\n%s", "Không kết nối được\n%s\nHãy kiểm tra mật khẩu.",
                           "Cài đặt", "Nhập mã này trên điện thoại", "Mã ghép nối",
                           "BOOT: Cài đặt Wi-Fi", "Giữ BOOT+PWR: thoát", "Kết nối %s\nMật khẩu %s\nrồi mở %s",
                           "Nhấn phím bất kỳ: đóng", "Đã cập nhật cài đặt", "Đã lưu Wi-Fi",
                           "Đồng hồ", "Nhiệt độ", "Ngôn ngữ", "Nội dung tin", "Ẩn số", "Xoay màn hình", "Thời tiết",
                           "Ghép nối", "Cài đặt Wi-Fi", "Bật", "Tắt",
                           "Điện thoại", "Mạng Wi-Fi", "Thông tin", "Xóa?", "Chưa có điện thoại", "Chưa có mạng",
                           "Điện thoại %u", "đang kết nối", "Phần mềm", "Thiết bị", "Bo mạch", "Pin", "Giao thức",
                           "Kiểm tra cập nhật", "Cập nhật", "Có bản cập nhật",
                           "Chưa kiểm tra", "Đang kiểm tra...", "Đã là mới nhất", "Bản mới", "Đã bỏ qua", "Đang cập nhật", "Cần app mới hơn", "Dung lượng",
                           "Cài đặt", "Kiểm tra ngay", "Nhắc lại sau", "Bỏ qua bản này", "Hủy",
                           {"", "Chưa lưu Wi-Fi", "Không có kết nối", "Chưa đặt giờ", "Tải về lỗi", "Bản cập nhật bị từ chối",
                            "Điện thoại đang cập nhật", "Đã hủy", "Pin dưới 30%", "Cập nhật lỗi"},
                           "Mặt đồng hồ", "Cổ điển", "Đang dùng", "Từ tệp", "Xóa", "Xóa?"},
};

ui_wifi_icon_t ui_wifi_icon_for(uint8_t wifi_state)
{
    /* The radio is off between fetches: the icon says "connected now", nothing else (user, 2026-10-15) */
    return wifi_state == PROTO_WIFI_STATE_CONNECTED ? UI_WIFI_OK : UI_WIFI_NONE;
}

int ui_weather_icon_for(uint8_t cond, bool day)
{
    switch (cond) {
    case WX_COND_CLEAR: return day ? UI_WEATHER_ICON_SUN : UI_WEATHER_ICON_MOON;
    case WX_COND_MOSTLY_CLEAR: return day ? UI_WEATHER_ICON_SUN_SMALL_CLOUD : UI_WEATHER_ICON_MOON;
    case WX_COND_PARTLY_CLOUDY: return day ? UI_WEATHER_ICON_SUN_CLOUD : UI_WEATHER_ICON_CLOUD;
    case WX_COND_OVERCAST: return UI_WEATHER_ICON_CLOUD;
    case WX_COND_FOG: return UI_WEATHER_ICON_FOG;
    case WX_COND_DRIZZLE: case WX_COND_RAIN: return UI_WEATHER_ICON_RAIN;
    case WX_COND_HEAVY_RAIN: return UI_WEATHER_ICON_HEAVY_RAIN;
    case WX_COND_SHOWERS: return day ? UI_WEATHER_ICON_SUN_RAIN : UI_WEATHER_ICON_RAIN;
    case WX_COND_SNOW: return UI_WEATHER_ICON_SNOW;
    case WX_COND_THUNDER: return UI_WEATHER_ICON_THUNDER;
    default: return -1;
    }
}

void ui_wifi_qr_data(char *out, size_t cap, const char *ssid, const char *pw)
{
    size_t n = 0;
    const char *parts[] = {"WIFI:T:WPA;S:", ssid, ";P:", pw, ";;"};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (const char *p = parts[i]; *p && n + 2 < cap; p++) {
            if (i % 2 == 1 && strchr("\\;,:\"", *p)) {
                out[n++] = '\\';
            }
            out[n++] = *p;
        }
    }
    out[n] = '\0';
}

const ui_strings_t *ui_strings(void)
{
    return &STRINGS[ui.cfg.language == PROTO_LANGUAGE_VI ? PROTO_LANGUAGE_VI : PROTO_LANGUAGE_EN];
}

#define DIM_PCT 20 /* dimmed backlight level (never above display.brightness) */

static uint32_t tick_cb(void) { return ui.p.now_ms ? ui.p.now_ms(ui.p.ctx) : 0; }

uint32_t ui_now_ms(void) { return tick_cb(); }

void ui_emit(ui_action_t action, uint32_t arg)
{
    if (ui.p.on_action) {
        ui.p.on_action(ui.p.ctx, action, arg);
    }
}

bool ui_step_card(int dir)
{
    if (ui.n_cards < 2) {
        return false;
    }
    ui.cur = dir > 0 ? (ui.cur + 1) % ui.n_cards : (ui.cur + ui.n_cards - 1) % ui.n_cards;
    ui.model_dirty = true;
    return true;
}

/* ---------------------------------------------------------------- backlight and touch (LCD) */

static bool has_backlight(void) { return ui.p.display->set_backlight != NULL; }

/* True if the screen was off: the event that woke it must not act as well. */
static bool wake(void)
{
    ui.last_activity_ms = ui_now_ms();
    ui.force_off = false;
    return ui.panel_off;
}

static void backlight_policy(void)
{
    const board_display_t *d = ui.p.display;
    if (!has_backlight() || !ui.shown_once) {
        return;
    }
    const uint32_t idle = ui_now_ms() - ui.last_activity_ms;
    uint8_t want = ui.cfg.brightness;
    if (ui.force_off || (ui.cfg.idle_off_s && idle >= ui.cfg.idle_off_s * 1000u)) {
        if (!ui.panel_off) {
            d->set_backlight(d->ctx, 0);
            d->sleep(d->ctx);
            ui.backlight = 0;
            ui.panel_off = true;
            if (ui.settings) { /* the page is not there when the screen comes back */
                ui.settings = false;
                ui.model_dirty = true;
            }
        }
        return;
    }
    if (ui.cfg.idle_dim_s && idle >= ui.cfg.idle_dim_s * 1000u && want > DIM_PCT) {
        want = DIM_PCT;
    }
    if (ui.panel_off) {
        /* the panel kept its picture, but the model may have changed while it slept */
        d->wake(d->ctx);
        ui.panel_off = false;
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(ui.disp);
    }
    if (want != ui.backlight) {
        d->set_backlight(d->ctx, want);
        ui.backlight = want;
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static bool swallow; /* the touch that woke the screen, until the finger lifts */
    board_touch_t t = {0};
    data->state = LV_INDEV_STATE_RELEASED;
    if (!ui.p.read_touch(ui.p.touch_ctx, &t) || !t.pressed) {
        swallow = false;
        return;
    }
    if (wake()) {
        swallow = true;
    }
    if (!swallow) {
        data->point.x = t.x;
        data->point.y = t.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

void ui_set_hidden(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return; /* LVGL redraws the object even when the flag does not change */
    }
    if (hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_mask_numbers(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap;) {
        if (!isdigit((unsigned char)src[i])) {
            dst[o++] = src[i++];
            continue;
        }
        size_t j = i, digits = 0;
        for (;;) {
            while (isdigit((unsigned char)src[j])) {
                j++;
                digits++;
            }
            if ((src[j] == '.' || src[j] == ',') && isdigit((unsigned char)src[j + 1])) {
                j++;
            } else {
                break;
            }
        }
        /* the output is never ahead of the input (4 bytes written for 4 or more read), so dst may be src */
        for (size_t k = 0; k < (digits >= 4 ? 4 : j - i) && o + 1 < cap; k++) {
            dst[o++] = digits >= 4 ? '*' : src[i + k];
        }
        i = j;
    }
    dst[o] = '\0';
}

const char *ui_card_text(char *buf, size_t cap, const char *src)
{
    if (!ui.cfg.mask_numbers) {
        return src;
    }
    ui_mask_numbers(buf, cap, src);
    return buf;
}

void ui_set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

const npipe_notif_t *ui_current_card(void)
{
    return ui.powered && ui.n_cards && ui.show_cards ? &ui.cards[ui.cur] : NULL;
}

bool ui_clock_shown(void)
{
    return ui.powered && ui_current_card() == NULL && ui.setup.state == UI_SETUP_OFF && !ui.st.pairing_mode &&
           !ui.st.has_passkey && !ui.settings && ui.notice == UI_NOTICE_NONE;
}

bool ui_in_use(void) { return !ui_clock_shown() || ui_now_ms() - ui.last_activity_ms < UI_FULL_QUIET_MS; }

static int64_t local_s(void) { return (int64_t)ui.utc + (int64_t)ui.cfg.tz_offset_min * 60; }

void ui_format_time(char *buf, size_t cap, const char **ampm, bool padded)
{
    board_datetime_t t;
    board_datetime_from_s(local_s(), &t);
    int h = t.hour;
    *ampm = NULL;
    if (!ui.cfg.clock_24h) {
        *ampm = h < 12 ? "AM" : "PM";
        h = h % 12 ? h % 12 : 12;
        padded = false;
    }
    snprintf(buf, cap, padded ? "%02d:%02d" : "%d:%02d", h, t.min);
}

bool ui_local_time(board_datetime_t *t)
{
    if (!ui.utc) {
        return false;
    }
    board_datetime_from_s(local_s(), t);
    return true;
}

void ui_format_date(char *buf, size_t cap)
{
    const ui_strings_t *s = ui_strings();
    board_datetime_t t;
    board_datetime_from_s(local_s(), &t);
    if (s->numeric_date) {
        snprintf(buf, cap, "%s, %d/%d", s->wday[t.wday], t.day, t.month);
    } else {
        snprintf(buf, cap, "%s, %d %s", s->wday[t.wday], t.day, s->month[t.month - 1]);
    }
}

int32_t ui_temp_shown(int32_t temp_c10)
{
    if (ui.cfg.temp_unit == PROTO_TEMP_UNIT_FAHRENHEIT) {
        temp_c10 = temp_c10 * 9 / 5 + 320;
    }
    return (temp_c10 >= 0 ? temp_c10 + 5 : temp_c10 - 5) / 10;
}

void ui_climate_shown(const board_climate_t *c, int32_t *temp, int32_t *rh)
{
    int32_t centi = c->temp_centi_c;
    if (ui.cfg.temp_unit == PROTO_TEMP_UNIT_FAHRENHEIT) {
        centi = centi * 9 / 5 + 3200;
    }
    *temp = (centi >= 0 ? centi + 5 : centi - 5) / 10;
    *rh = ((int32_t)c->rh_centi_pct + 50) / 100;
}

/* ---------------------------------------------------------------- public API */

bool ui_init(const ui_params_t *p)
{
    ui_deinit();
    ui.p = *p;
    p->display->get_info(p->display->ctx, &ui.info);
    if (UI_HAS_MONO && ui.info.color_depth == 1 && ui.info.width % 8 == 0) {
#if UI_HAS_MONO
        ui.backend = &ui_backend_frame;
        ui.layout = &ui_layout_mono;
#endif
    } else if (UI_HAS_COLOUR && ui.info.color_depth == 16 && p->display->write_area) {
#if UI_HAS_COLOUR
        ui.backend = &ui_backend_area;
        ui.layout = &ui_layout_colour;
#endif
    } else {
        return false;
    }
    if (!p->display->init(p->display->ctx)) {
        return false;
    }
    ui.cfg = (ui_config_t){.language = PROTO_LANGUAGE_EN, .show_body = true, .full_refresh_every = 30,
                           .clock_24h = true, .temp_unit = PROTO_TEMP_UNIT_CELSIUS, .brightness = 70,
                           .idle_dim_s = 30, .idle_off_s = 60, .weather_enabled = true};
    ui.shown_minute = TIME_UNKNOWN;
    ui.st = (ui_status_t){.battery_pct = 255, .ota_pct = -1};
    ui.powered = true;
    ui.show_cards = true;

    lv_init();
    lv_tick_set_cb(tick_cb);
    if (!ui.backend->init()) {
        ui_deinit();
        return false;
    }
    if (p->read_touch) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
    }
    ui.layout->build();
    ui.model_dirty = true;
    ui.last_activity_ms = ui_now_ms();
    return true;
}

void ui_deinit(void)
{
    ui_face_reset();
    if (ui.disp) {
        lv_deinit(); /* frees every LVGL allocation, including the backend's buffers */
    }
    if (ui.backend) {
        ui.backend->reset();
    }
    if (ui.layout) {
        ui.layout->reset();
    }
    memset(&ui, 0, sizeof(ui));
}

void ui_set_config(const ui_config_t *c)
{
    const board_display_t *d = ui.p.display;
    if ((d->set_rotation || d->write_frame) && c->orientation != ui.cfg.orientation && ui.disp) {
        if (d->set_rotation) { /* the LCD driver turns the panel; the frame backend turns the picture itself */
            d->set_rotation(d->ctx, c->orientation);
        }
        ui.cfg.orientation = c->orientation; /* before the redraw below: the frame backend reads it */
        lv_obj_invalidate(lv_screen_active()); /* the picture was turned: draw it all again, now */
        ui.model_dirty = true;
    }
    if (c->language != ui.cfg.language || c->show_body != ui.cfg.show_body || c->mask_numbers != ui.cfg.mask_numbers ||
        c->tz_offset_min != ui.cfg.tz_offset_min || c->clock_24h != ui.cfg.clock_24h ||
        c->temp_unit != ui.cfg.temp_unit || c->weather_enabled != ui.cfg.weather_enabled ||
        c->update_auto_check != ui.cfg.update_auto_check || c->clock_face_opts_len != ui.cfg.clock_face_opts_len ||
        strcmp(c->clock_face_id, ui.cfg.clock_face_id) != 0 ||
        memcmp(c->clock_face_opts, ui.cfg.clock_face_opts, sizeof(c->clock_face_opts)) != 0) {
        ui.model_dirty = true;
    }
    ui.cfg = *c;
    ui.cfg.clock_face_id[sizeof(ui.cfg.clock_face_id) - 1] = '\0';
    ui_settings_config_changed();
}

void ui_set_update(const ui_update_t *u)
{
    if (memcmp(u, &ui.upd, sizeof(*u)) == 0) {
        return;
    }
    ui.upd = *u;
    if (ui.upd_prompt && ui.settings && ui.set_page == UI_PAGE_UPDATE &&
        (ui.upd.state != UI_UPD_AVAILABLE || ui.upd.skipped)) {
        ui.upd_prompt = false; /* skipped or installed from somewhere else: the question is gone, the page stays */
    }
    if (ui.settings && ui.set_page == UI_PAGE_UPDATE) {
        ui_settings_clamp(); /* the page's rows follow the state */
    }
    ui.model_dirty = true;
}

bool ui_show_update_prompt(void)
{
    if (!ui.powered || ui.settings || !ui.disp || ui.panel_off || !ui_clock_shown() || ui.upd.state != UI_UPD_AVAILABLE ||
        ui.upd.skipped) {
        return false;
    }
    ui.settings = true;
    ui.set_page = UI_PAGE_UPDATE;
    ui.set_cur = ui.set_top = ui.set_main_cur = 0;
    ui.set_confirm = false;
    ui.upd_prompt = true;
    ui.set_cur = 1; /* Install: the row after the status */
    ui_settings_clamp();
    ui.last_activity_ms = ui_now_ms(); /* the settings time out from here, not from the last key */
    ui.model_dirty = true;
    return true;
}

void ui_set_status(const ui_status_t *s)
{
    if (memcmp(s, &ui.st, sizeof(*s)) != 0) {
        if (s->pairing_mode && !ui.st.pairing_mode && ui.settings) {
            ui.settings = false; /* pairing opened (the last phone was forgotten, or the keys): its screen, then the clock */
        }
        ui.st = *s;
        ui.model_dirty = true;
    }
}

void ui_set_time(uint32_t utc_s)
{
    ui.utc = utc_s;
    const uint32_t minute = utc_s ? utc_s / 60u : TIME_UNKNOWN; /* time zones are whole minutes */
    if (minute != ui.shown_minute) {
        ui.shown_minute = minute;
        ui.model_dirty = true;
    }
}

void ui_set_weather(const wx_reading_t *r)
{
    if (r == NULL) {
        ui.model_dirty |= ui.wx_ok;
        ui.wx_ok = false;
        return;
    }
    /* compare what is shown, not the raw reading: a 0.01 deg change does not redraw */
    const wx_reading_t *o = &ui.wx;
    bool same = ui.wx_ok && r->cond == o->cond && r->is_day == o->is_day && r->precip_pct == o->precip_pct &&
                ui_temp_shown(r->temp_c10) == ui_temp_shown(o->temp_c10) &&
                (r->high_c10 == WX_TEMP_UNKNOWN) == (o->high_c10 == WX_TEMP_UNKNOWN) &&
                (r->low_c10 == WX_TEMP_UNKNOWN) == (o->low_c10 == WX_TEMP_UNKNOWN) &&
                ui_temp_shown(r->high_c10) == ui_temp_shown(o->high_c10) &&
                ui_temp_shown(r->low_c10) == ui_temp_shown(o->low_c10) && strcmp(r->place, o->place) == 0;
    ui.wx = *r;
    ui.wx_ok = true;
    ui.model_dirty |= !same;
}

void ui_set_wifi_setup(const ui_wifi_setup_t *s)
{
    if (memcmp(s, &ui.setup, sizeof(*s)) != 0) {
        ui.setup = *s;
        ui.model_dirty = true;
        wake(); /* the user is looking at it */
    }
}

void ui_show_notice(ui_notice_t n)
{
    ui.notice = n;
    ui.notice_ms = ui_now_ms();
    ui.model_dirty = true;
    wake(); /* somebody just changed it: look */
}

ui_notice_t ui_current_notice(void) { return ui.notice; }

void ui_set_climate(const board_climate_t *c)
{
    /* redraw only when the rounded text changes: sensor noise below 0.1 degree costs no refresh */
    int32_t t_new = 0, rh_new = 0, t_old = 0, rh_old = 0;
    if (c) {
        ui_climate_shown(c, &t_new, &rh_new);
    }
    if (ui.climate_ok) {
        ui_climate_shown(&ui.clim, &t_old, &rh_old);
    }
    if ((c != NULL) != ui.climate_ok || t_new != t_old || rh_new != rh_old) {
        ui.model_dirty = true;
    }
    ui.climate_ok = c != NULL;
    if (c) {
        ui.clim = *c;
    }
}

static int card_index(uint32_t id)
{
    for (size_t i = 0; i < ui.n_cards; i++) {
        if (ui.cards[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* The queue of waiting notifications (D81). */
static void queue_drop(uint32_t id)
{
    for (size_t i = 0; i < ui.n_queue; i++) {
        if (ui.queue[i] == id) {
            memmove(&ui.queue[i], &ui.queue[i + 1], (ui.n_queue - i - 1) * sizeof(ui.queue[0]));
            ui.n_queue--;
            return;
        }
    }
}

/* Open the oldest waiting notification that still has a card; false when none is left. */
static bool queue_open_next(void)
{
    while (ui.n_queue) {
        const int k = card_index(ui.queue[0]);
        memmove(&ui.queue[0], &ui.queue[1], (ui.n_queue - 1) * sizeof(ui.queue[0]));
        ui.n_queue--;
        if (k >= 0) {
            ui.cur = (size_t)k;
            ui.show_cards = true;
            ui.model_dirty = true;
            return true;
        }
    }
    return false;
}

void ui_on_notification(const npipe_notif_t *n)
{
    int i = card_index(n->id);
    if (n->kind == NPIPE_REMOVED) {
        if (i < 0) {
            return;
        }
        const bool shown_gone = ui.cards[i].id == ui.shown_id;
        queue_drop(ui.cards[i].id);
        memmove(&ui.cards[i], &ui.cards[i + 1], (ui.n_cards - (size_t)i - 1) * sizeof(ui.cards[0]));
        ui.n_cards--;
        if (ui.cur > (size_t)i || ui.cur >= ui.n_cards) {
            ui.cur = ui.cur ? ui.cur - 1 : 0;
        }
        if (ui.n_queue && shown_gone) { /* the card that was held on screen is gone: the next one opens at once */
            (void)queue_open_next();
        }
        if (ui.n_cards == 0) {
            ui.show_cards = true; /* the next notification opens its card */
        }
    } else if (i >= 0 && n->kind == NPIPE_MODIFIED) {
        ui.cards[i] = *n;
    } else {
        if (i >= 0) { /* re-added: move to the front */
            memmove(&ui.cards[i], &ui.cards[i + 1], (ui.n_cards - (size_t)i - 1) * sizeof(ui.cards[0]));
            ui.n_cards--;
        }
        size_t keep = ui.n_cards < UI_MAX_CARDS ? ui.n_cards : UI_MAX_CARDS - 1;
        memmove(&ui.cards[1], &ui.cards[0], keep * sizeof(ui.cards[0]));
        ui.cards[0] = *n;
        ui.n_cards = keep + 1;
        /* A card that reached the panel less than UI_CARD_MIN_MS ago is not replaced yet, and neither is one that
         * has others waiting behind it (D81): it keeps the screen (the counter shows there are newer ones), the
         * new notification joins the queue and ui_service opens the queued ones in turn, each for its minimum time. */
        queue_drop(n->id);
        const bool held = ui.shown_id != 0 && ui.shown_id != n->id && !ui.settings &&
                          (ui.n_queue > 0 || !ui.card_since_known || ui_now_ms() - ui.card_since_ms < UI_CARD_MIN_MS);
        const int shown = held ? card_index(ui.shown_id) : -1;
        if (shown >= 0) {
            ui.cur = (size_t)shown;
            if (ui.n_queue < UI_MAX_CARDS) {
                ui.queue[ui.n_queue++] = n->id;
            }
        } else {
            ui.cur = 0; /* show the newest */
            ui.show_cards = true; /* a new notification opens its card, also from the clock screen */
            ui.n_queue = 0;
        }
        ui.settings = false; /* a new notification replaces the settings page */
        wake();
    }
    ui.model_dirty = true;
}

void ui_assets_changed(uint8_t asset_type)
{
    if (asset_type == PROTO_ASSET_TYPE_APP_ICON || asset_type == PROTO_ASSET_TYPE_CLOCK_FACE) {
        ui.assets_gen++;
        ui.model_dirty = true; /* an unchanged frame is not refreshed, so this costs nothing when no card is affected */
        if (asset_type == PROTO_ASSET_TYPE_CLOCK_FACE) {
            ui_settings_faces_changed();
        }
    }
}

ui_icon_t ui_current_icon(void) { return ui.icon_src; }
ui_link_icon_t ui_link_icon(void) { return ui.link_src; }
ui_wifi_icon_t ui_wifi_icon(void) { return ui.wifi_src; }

ui_action_t ui_on_button(board_button_t btn, board_press_t press)
{
    if (btn == BOARD_BTN_UP_DOWN) {
        if (wake()) {
            return UI_ACTION_NONE;
        }
        if (press == BOARD_PRESS_SHORT) { /* both keys pressed together: sleep now (any key wakes it) */
            ui.force_off = has_backlight();
            return UI_ACTION_NONE;
        }
        return UI_ACTION_PAIRING_TOGGLE;
    }
    if (btn == BOARD_BTN_UP || btn == BOARD_BTN_DOWN) {
        /* LCD boards (PLAN 7c): short = previous/next card, long = brightness; a press that wakes
         * the screen does nothing else; any key closes the Wi-Fi setup screen (D61) and the settings page */
        if (wake()) {
            return UI_ACTION_NONE;
        }
        if (ui.setup.state != UI_SETUP_OFF) {
            return UI_ACTION_WIFI_SETUP_CLOSE;
        }
        if (ui.settings) {
            ui.settings = false;
            ui.model_dirty = true;
            return UI_ACTION_NONE;
        }
        if (press == BOARD_PRESS_LONG) {
            return btn == BOARD_BTN_UP ? UI_ACTION_BRIGHTNESS_UP : UI_ACTION_BRIGHTNESS_DOWN;
        }
        if (!ui.show_cards && ui.n_cards) { /* from the clock screen a key brings the cards back first */
            ui.show_cards = true;
            ui.model_dirty = true;
            return UI_ACTION_NONE;
        }
        ui_step_card(btn == BOARD_BTN_UP ? -1 : 1);
        return UI_ACTION_NONE;
    }
    wake(); /* a key press counts as activity: the cards' timeout starts again (D80) */
    if (btn == BOARD_BTN_PWR && press == BOARD_PRESS_HOLD) {
        return UI_ACTION_POWER_TOGGLE; /* from anywhere, PLAN D96; long PWR (1.5 s) is "back" and comes first */
    }
    if (!ui.powered) {
        return UI_ACTION_NONE;
    }
    if (btn == BOARD_BTN_BOOT_PWR) {
        return press == BOARD_PRESS_LONG ? UI_ACTION_PAIRING_TOGGLE : UI_ACTION_NONE;
    }
    if (ui.setup.state != UI_SETUP_OFF) { /* the Wi-Fi setup screen: any key closes it (D61) */
        return UI_ACTION_WIFI_SETUP_CLOSE;
    }
    if (ui.st.pairing_mode || ui.st.has_passkey) { /* the pairing screen: short BOOT opens the Wi-Fi setup (D78) */
        return btn == BOARD_BTN_BOOT && press == BOARD_PRESS_SHORT && ui.p.wifi_setup && !ui.st.has_passkey
                   ? UI_ACTION_WIFI_SETUP
                   : UI_ACTION_NONE;
    }
    if (ui.settings) { /* the settings list (D96): short BOOT up, short PWR down, long BOOT select, long PWR back */
        if (press == BOARD_PRESS_SHORT) {
            ui_settings_step(btn == BOARD_BTN_BOOT ? -1 : 1);
        } else if (btn == BOARD_BTN_BOOT) {
            return ui_settings_select();
        } else {
            ui_settings_back();
        }
        return UI_ACTION_NONE;
    }
    if (press != BOARD_PRESS_SHORT) {
        return UI_ACTION_NONE;
    }
    if (btn == BOARD_BTN_PWR && !(ui.n_cards && ui.show_cards)) { /* short PWR on the clock: the settings (D98) */
        ui_settings_enter();
        return UI_ACTION_NONE;
    }
    if (ui.n_cards) {
        /* BOOT = older card, PWR = newer (PLAN D74, D78). Stepping past the last or the first card shows the
         * clock; from the clock short BOOT opens the cards at the newest (D98: short PWR opens the settings). */
        const bool older = btn == BOARD_BTN_BOOT;
        if (!ui.show_cards) {
            ui.show_cards = true;
            ui.cur = 0;
        } else if (older ? ui.cur + 1 >= ui.n_cards : ui.cur == 0) {
            ui.show_cards = false;
        } else {
            ui.cur = older ? ui.cur + 1 : ui.cur - 1;
        }
        ui.model_dirty = true;
    }
    return UI_ACTION_NONE;
}

void ui_activity(void) { wake(); }

uint8_t ui_backlight(void) { return ui.backlight; }

bool ui_screen_off(void) { return ui.panel_off; }

uint8_t ui_brightness_step(uint8_t brightness, bool up)
{
    const int v = ((int)brightness + 5) / 10 * 10 + (up ? 10 : -10); /* to the nearest step, then one step */
    return (uint8_t)(v < 10 ? 10 : v > 100 ? 100 : v);
}

void ui_set_power(bool on)
{
    if (on != ui.powered) {
        ui.powered = on;
        ui.model_dirty = true;
    }
}

bool ui_service(void)
{
    lv_timer_handler();
    if (ui.model_dirty) {
        ui.model_dirty = false;
        ui.layout->apply();
        lv_refr_now(ui.disp);
    }
    const bool pending = ui.backend->service();
    backlight_policy();
    /* How long the card on screen has been on the panel; a newer notification waits for UI_CARD_MIN_MS of it */
    const npipe_notif_t *card = ui_current_card();
    const uint32_t shown_id = card && ui.setup.state == UI_SETUP_OFF && !ui.st.pairing_mode && !ui.st.has_passkey &&
                                      !ui.settings
                                  ? card->id
                                  : 0;
    if (shown_id != ui.shown_id) {
        ui.shown_id = shown_id;
        ui.card_since_known = false;
    }
    if (shown_id && !pending && !ui.card_since_known) {
        ui.card_since_known = true;
        ui.card_since_ms = ui_now_ms();
    }
    if (ui.n_queue && (!shown_id || (ui.card_since_known && ui_now_ms() - ui.card_since_ms >= UI_CARD_MIN_MS))) {
        (void)queue_open_next();
    }
    if (ui.notice != UI_NOTICE_NONE && ui_now_ms() - ui.notice_ms >= UI_NOTICE_MS) {
        ui.notice = UI_NOTICE_NONE;
        ui.model_dirty = true;
    }
    if (ui.settings && ui.p.cards_timeout_s &&
        ui_now_ms() - ui.last_activity_ms >= ui.p.cards_timeout_s * 1000u) {
        ui.settings = false; /* nobody is choosing: back to the clock (D96) */
        ui.model_dirty = true;
    }
    if (ui.show_cards && ui.n_cards && ui.p.cards_timeout_s &&
        ui_now_ms() - ui.last_activity_ms >= ui.p.cards_timeout_s * 1000u) {
        ui.show_cards = false; /* nobody is reading the card: back to the clock */
        ui.model_dirty = true;
    }
    /* a message or a queued card is on a timer of a few seconds: keep calling (the app does not sleep meanwhile) */
    const bool timers = ui.model_dirty || ui.n_queue != 0 || ui.notice != UI_NOTICE_NONE;
    ui.panel_wait = pending && !timers && ui.backend->panel_wait != NULL && ui.backend->panel_wait();
    return pending || timers;
}

bool ui_panel_wait(void) { return ui.panel_wait; }

void ui_get_stats(ui_stats_t *out) { *out = ui.stats; }

size_t ui_visible_text(char *out, size_t cap)
{
    if (!cap) {
        return 0;
    }
    out[0] = 0;
    if (!ui.disp) {
        return 0;
    }
    size_t n = ui.layout->visible_text(out, cap);
    return n + ui_face_text(out + n, cap - n);
}
