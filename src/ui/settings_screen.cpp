#include "settings_screen.h"

#include "board/board.h"
#include "nav.h"
#include "net/commands.h"
#include "app_settings.h"
#include "widgets.h"

namespace ui::settings_screen {

static Layout layout;
static lv_obj_t *scr;
static lv_obj_t *sliders[3];  // settings::Value order; nullptr when the board lacks it
static lv_obj_t *info_values[9];

static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_BOTTOM) nav::to_devices();
}

static void on_back(lv_event_t *) { nav::to_devices(); }

static void on_slider(lv_event_t *e) {
    auto which = (settings::Value)(intptr_t)lv_event_get_user_data(e);
    settings::preview(which, lv_slider_get_value(lv_event_get_target(e)));
}

static void section(lv_obj_t *parent, const char *title) {
    lv_obj_t *t = widgets::label(parent, title, layout.font_small, color::muted());
    lv_obj_set_style_pad_top(t, layout.margin, 0);
    lv_obj_set_style_pad_bottom(t, layout.gap / 2, 0);
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *title, settings::Value which, int min) {
    lv_obj_t *r = widgets::box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, layout.gap, 0);
    lv_obj_set_style_pad_ver(r, layout.gap, 0);
    widgets::label(r, title, layout.font_body, color::text());

    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_height(s, 8);
    lv_slider_set_range(s, min, 100);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, color::text(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, color::text(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, layout.gap);
    // A slider would otherwise eat the swipe-down that leaves the screen
    lv_obj_clear_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s, on_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)which);
    return s;
}

static lv_obj_t *info_row(lv_obj_t *parent, const char *title) {
    lv_obj_t *r = widgets::box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, layout.gap / 2, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
    lv_obj_t *t = widgets::label(r, title, layout.font_small, color::muted());
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = widgets::label(r, "...", layout.font_small, color::text());
    lv_obj_set_width(v, lv_pct(60));
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

void build(const Layout &l) {
    layout = l;
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    widgets::on_gesture(scr, on_gesture);

    // Header like the device list's: title, and the same round button to go back up a level
    lv_obj_t *h = widgets::box(scr);
    lv_obj_set_size(h, l.width - 2 * l.margin, l.btn);
    lv_obj_set_pos(h, l.margin, l.margin / 2);
    lv_obj_t *title = widgets::label(h, "Settings", l.font_title, color::text());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *back = widgets::round_button(h, LV_SYMBOL_DOWN, l.btn_small, l.font_body, on_back);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_coord_t top = l.margin / 2 + l.btn;
    lv_obj_set_pos(body, l.margin, top);
    lv_obj_set_size(body, l.width - 2 * l.margin, l.height - top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(body, l.margin, 0);

    const board::Info &b = board::info();
    section(body, "DISPLAY");
    sliders[(int)settings::Value::Brightness] =
        slider_row(body, "Screen brightness", settings::Value::Brightness, 10);
    if (b.has_led) {
        sliders[(int)settings::Value::LedBrightness] =
            slider_row(body, "LED brightness", settings::Value::LedBrightness, 0);
    }
    if (b.has_speaker) {
        section(body, "SOUND");
        sliders[(int)settings::Value::TapVolume] = slider_row(body, "Tap sound", settings::Value::TapVolume, 0);
    }

    section(body, "INFO");
    static const char *kInfo[] = {"Screen", "Firmware", "WiFi", "IP address", "Settings page", "Settings password",
                                  "Server", "MQTT", "Battery"};
    int rows = b.has_battery ? 9 : 8;
    for (int i = 0; i < rows; i++) info_values[i] = info_row(body, kInfo[i]);
}

lv_obj_t *screen() { return scr; }

void open() {
    for (int i = 0; i < 3; i++) {
        if (sliders[i]) lv_slider_set_value(sliders[i], settings::value((settings::Value)i), LV_ANIM_OFF);
    }
    commands::post(commands::Type::LoadSettingsInfo, -1);
}

void set_info(const SettingsInfo &info) {
    const char *values[] = {info.board, info.firmware, info.wifi,   info.ip,
                            info.settings_url, info.password, info.server, info.mqtt, info.battery};
    for (int i = 0; i < 9; i++)
        if (info_values[i]) lv_label_set_text(info_values[i], values[i]);
}

}  // namespace ui::settings_screen
