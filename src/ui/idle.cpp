#include "idle.h"

#include <time.h>

#include "nav.h"
#include "widgets.h"

namespace ui::idle {

static lv_obj_t *scr;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *device_label;

static void update_clock(lv_timer_t * = nullptr) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    // No time yet (NTP not synced): leave the clock out rather than show 1970
    bool known = t.tm_year + 1900 >= 2024;
    if (!known) {
        lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(date_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char buf[40];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    lv_label_set_text(clock_label, buf);
    strftime(buf, sizeof(buf), "%A %e %B", &t);
    lv_label_set_text(date_label, buf);
    lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(date_label, LV_OBJ_FLAG_HIDDEN);
}

// Nothing is playing: pick another device (up) or start something (down)
static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_TOP) nav::to_devices();
    else if (dir == LV_DIR_BOTTOM) nav::to_library();
}

void build(const Layout &l) {
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scr, l.gap, 0);
    widgets::on_gesture(scr, on_gesture);

    clock_label = widgets::label(scr, "", l.font_code, color::text());
    lv_obj_set_style_text_letter_space(clock_label, 4, 0);
    date_label = widgets::label(scr, "", l.font_body, color::muted());
    device_label = widgets::label(scr, "", l.font_small, color::muted());
    lv_obj_set_style_pad_top(device_label, l.margin, 0);

    update_clock();
    lv_timer_create(update_clock, 1000, nullptr);
}

lv_obj_t *screen() { return scr; }

static const char *state_text(PlayState s) {
    switch (s) {
        case PlayState::Standby:     return "Standby";
        case PlayState::Unreachable: return "Not reachable";
        case PlayState::Paused:      return "Paused";
        default:                     return "Nothing playing";
    }
}

void set_device(const Device *d) {
    if (!d) {
        lv_label_set_text(device_label, "No device");
        return;
    }
    lv_label_set_text_fmt(device_label, "%s  \xC2\xB7  %s", d->name, state_text(d->state));
}

}  // namespace ui::idle
