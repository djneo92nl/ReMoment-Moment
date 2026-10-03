#include "volume_screen.h"

#include <Arduino.h>

#include "nav.h"
#include "net/commands.h"
#include "ui.h"
#include "widgets.h"

namespace ui::volume_screen {

static const uint32_t kEchoGuardMs = 1500;  // ignore server echoes right after a local change

// What this screen shows of the current device; updated optimistically on taps
static struct {
    int      id     = -1;
    uint16_t caps   = 0;
    int      volume = -1;
    bool     muted  = false;
    bool     on     = true;
} shown;

static Layout layout;
static lv_obj_t *scr;
static lv_obj_t *title_label;
static lv_obj_t *power_btn;
static lv_obj_t *mute_btn;
static lv_obj_t *slider;
static lv_obj_t *value_label;
static lv_obj_t *source_box;  // header + label, hidden without a known source
static lv_obj_t *source_label;
static uint32_t  last_local_ms = 0;

static void set_visible(lv_obj_t *obj, bool visible) {
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show_volume() {
    lv_label_set_text_fmt(value_label, shown.volume >= 0 ? "%d" : "-", shown.volume);
    if (shown.volume >= 0) lv_slider_set_value(slider, shown.volume, LV_ANIM_OFF);
    widgets::set_symbol(mute_btn, shown.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    widgets::set_active(mute_btn, shown.muted);
}

static void show_capabilities() {
    set_visible(power_btn, (shown.caps & kCapPower) != 0);
}

// ---- Events ---------------------------------------------------------------------------

static void on_back(lv_event_t *) { nav::to_now_playing(); }

static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_LEFT) nav::to_now_playing();
}

static void on_slider(lv_event_t *) {
    shown.volume = lv_slider_get_value(slider);
    lv_label_set_text_fmt(value_label, "%d", shown.volume);
    last_local_ms = lv_tick_get();
    commands::post(commands::Type::Volume, shown.id, shown.volume);
}

static void on_mute(lv_event_t *) {
    shown.muted   = !shown.muted;
    last_local_ms = lv_tick_get();
    commands::post(commands::Type::Mute, shown.id, shown.muted);
    show_volume();
}

static void on_power(lv_event_t *) {
    shown.on = !shown.on;
    widgets::set_active(power_btn, shown.on);
    commands::post(commands::Type::Power, shown.id, shown.on);
}

// ---- Build ----------------------------------------------------------------------------

static lv_obj_t *section(lv_obj_t *parent, const char *title) {
    lv_obj_t *t = widgets::label(parent, title, layout.font_small, color::muted());
    lv_obj_set_style_pad_top(t, layout.margin, 0);
    lv_obj_set_style_pad_bottom(t, layout.gap / 2, 0);
    return t;
}

void build(const Layout &l) {
    layout = l;
    scr    = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    widgets::on_gesture(scr, on_gesture);

    // Navigation bar: back to Now Playing on the left, device name, standby on the right
    lv_obj_t *h = widgets::box(scr);
    lv_obj_set_size(h, l.width - 2 * l.margin, l.btn);
    lv_obj_set_pos(h, l.margin, l.margin / 2);
    lv_obj_t *back = widgets::round_button(h, LV_SYMBOL_LEFT, l.btn_small, l.font_body, on_back);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    title_label = widgets::label(h, "", l.font_title, color::text());
    lv_obj_set_width(title_label, l.width - 2 * l.margin - 2 * (l.btn_small + l.gap));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, l.btn_small + l.gap, 0);
    power_btn = widgets::round_button(h, LV_SYMBOL_POWER, l.btn_small, l.font_body, on_power);
    lv_obj_align(power_btn, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_coord_t top = l.margin / 2 + l.btn + l.gap;
    lv_obj_set_pos(body, l.margin, top);
    lv_obj_set_size(body, l.width - 2 * l.margin, l.height - top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(body, l.margin, 0);

    section(body, "VOLUME");
    lv_obj_t *r = widgets::box(body);
    lv_obj_set_size(r, lv_pct(100), l.btn);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, l.margin, 0);

    mute_btn = widgets::round_button(r, LV_SYMBOL_VOLUME_MAX, l.btn, l.font_body, on_mute);

    slider = lv_slider_create(r);
    lv_obj_set_height(slider, 12);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color::text(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color::text(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, l.btn / 2);
    // A slider would otherwise eat the swipe that leaves the screen
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, nullptr);

    value_label = widgets::label(r, "-", l.font_title, color::text());
    lv_obj_set_width(value_label, l.font_title->line_height * 2);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);

    source_box = widgets::box(body);
    lv_obj_set_size(source_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(source_box, LV_FLEX_FLOW_COLUMN);
    section(source_box, "SOURCE");
    source_label = widgets::label(source_box, "", l.font_body, color::text());
    lv_obj_set_width(source_label, lv_pct(100));
    lv_label_set_long_mode(source_label, LV_LABEL_LONG_DOT);
    set_visible(source_box, false);

    show_volume();
    show_capabilities();
}

lv_obj_t *screen() { return scr; }

bool available() { return (shown.caps & kCapVolume) != 0; }

void open() {
    show_volume();
    show_capabilities();
    widgets::set_active(power_btn, shown.on);
    if (shown.id >= 0 && (shown.caps & (kCapSources | kCapSourceActivation))) {
        commands::post(commands::Type::LoadSource, shown.id);
    }
}

void update(const Device &d, uint8_t what) {
    if (d.id != shown.id) {
        shown.id = d.id;
        what     = kUpdateAll;
        set_visible(source_box, false);  // the previous device's source
    }

    if (what & kUpdateDevice) {
        lv_label_set_text(title_label, d.name);
        shown.caps = d.caps;
        show_capabilities();
    }
    if (what & kUpdateState) {
        shown.on = d.state != PlayState::Standby && d.state != PlayState::Unreachable;
        widgets::set_active(power_btn, shown.on);
    }
    if (what & kUpdateVolume) {
        // Don't let the server's echo fight the slider while it's being dragged
        bool recent_local = lv_tick_elaps(last_local_ms) < kEchoGuardMs;
        if (!recent_local || what == kUpdateAll) {
            shown.volume = d.volume;
            shown.muted  = d.muted;
            show_volume();
        }
    }
}

void set_source(int device_id, const char *label) {
    if (device_id != shown.id) return;
    bool any = label && *label;
    if (any) lv_label_set_text(source_label, label);
    set_visible(source_box, any);
}

}  // namespace ui::volume_screen
