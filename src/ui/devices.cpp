#include "devices.h"

#include <Arduino.h>

#include "nav.h"
#include "net/commands.h"
#include "widgets.h"

namespace ui::devices {

static const int kMaxOptions = 32;

static Layout layout;

// ---- Device list ------------------------------------------------------------------

struct Row {
    int       id;
    uint16_t  caps;
    PlayState state;
    lv_obj_t *obj, *name, *sub, *strip;
};

static lv_obj_t *list_scr;
static lv_obj_t *list_box;
static Row rows[kMaxDevices];
static uint8_t row_count = 0;

static const char *state_text(PlayState s) {
    switch (s) {
        case PlayState::Playing:     return "Playing";
        case PlayState::Paused:      return "Paused";
        case PlayState::Standby:     return "Standby";
        case PlayState::Unreachable: return "Not reachable";
        default:                     return "";
    }
}

static void fill_row(Row &r, const Device &d, bool current) {
    r.id    = d.id;
    r.caps  = d.caps;
    r.state = d.state;
    lv_label_set_text(r.name, d.name);
    if (d.active() && d.track[0]) {
        lv_label_set_text_fmt(r.sub, "%s - %s - %s", state_text(d.state), d.artist, d.track);
    } else {
        lv_label_set_text(r.sub, state_text(d.state));
    }
    if (current) lv_obj_clear_flag(r.strip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(r.strip, LV_OBJ_FLAG_HIDDEN);
}

static void on_row(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    commands::post(commands::Type::SelectDevice, id);
    nav::to_now_playing();
}

static void on_row_details(lv_event_t *e) {
    nav::to_details((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_close(lv_event_t *) { nav::to_now_playing(); }

static void on_list_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_BOTTOM) nav::to_now_playing();
    else if (dir == LV_DIR_TOP) nav::to_settings();
}

static lv_obj_t *header(lv_obj_t *parent, const char *title, const char *close_symbol, lv_event_cb_t on_click,
                        lv_obj_t **title_label = nullptr) {
    lv_obj_t *h = widgets::box(parent);
    lv_obj_set_size(h, layout.width - 2 * layout.margin, layout.btn);
    lv_obj_set_pos(h, layout.margin, layout.margin / 2);
    lv_obj_t *t = widgets::label(h, title, layout.font_title, color::text());
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    if (title_label) *title_label = t;
    lv_obj_t *close = widgets::round_button(h, close_symbol, layout.btn_small, layout.font_body, on_click);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    return h;
}

static lv_obj_t *scroll_column(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_coord_t top = layout.margin / 2 + layout.btn + layout.gap;
    lv_obj_set_pos(c, layout.margin, top);
    lv_obj_set_size(c, layout.width - 2 * layout.margin, layout.height - top);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(c, layout.margin, 0);
    return c;
}

static lv_obj_t *list_row(lv_obj_t *parent, lv_event_cb_t on_click, void *user_data, lv_obj_t **title,
                          lv_obj_t **detail) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, lv_pct(100), layout.row);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(r, color::text(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(r, layout.gap, 0);
    if (on_click) lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, user_data);

    *title = widgets::label(r, "", layout.font_body, color::text());
    lv_obj_align(*title, LV_ALIGN_LEFT_MID, 0, detail ? -layout.gap : 0);
    if (detail) {
        *detail = widgets::label(r, "", layout.font_small, color::muted());
        lv_obj_set_width(*detail, layout.width - 4 * layout.margin - layout.btn_small);
        lv_label_set_long_mode(*detail, LV_LABEL_LONG_DOT);
        lv_obj_align(*detail, LV_ALIGN_LEFT_MID, 0, layout.gap);
    }
    return r;
}

void sync(const Device *devices, uint8_t count, int current) {
    lv_obj_clean(list_box);
    row_count = 0;
    for (uint8_t i = 0; i < count && i < kMaxDevices; i++) {
        Row &r = rows[row_count++];
        void *id = (void *)(intptr_t)devices[i].id;
        r.obj = list_row(list_box, on_row, id, &r.name, &r.sub);

        // Current device: a thin accent strip on the left, nothing louder
        r.strip = widgets::box(r.obj);
        lv_obj_set_size(r.strip, 3, layout.row / 2);
        lv_obj_set_style_bg_color(r.strip, color::text(), 0);
        lv_obj_set_style_bg_opa(r.strip, LV_OPA_COVER, 0);
        lv_obj_align(r.strip, LV_ALIGN_LEFT_MID, -layout.gap, 0);

        lv_obj_t *more = widgets::round_button(r.obj, LV_SYMBOL_SETTINGS, layout.btn_small, layout.font_small,
                                               on_row_details, id);
        lv_obj_align(more, LV_ALIGN_RIGHT_MID, 0, 0);
        fill_row(r, devices[i], (int)i == current);
    }
    if (count == 0) widgets::label(list_box, "No devices assigned", layout.font_body, color::muted());
}

void update(int index, const Device &d) {
    if (index < 0 || index >= row_count || rows[index].id != d.id) return;
    bool current = !lv_obj_has_flag(rows[index].strip, LV_OBJ_FLAG_HIDDEN);
    fill_row(rows[index], d, current);
}

// ---- Details ----------------------------------------------------------------------

struct SectionUi {
    const char *title;
    uint16_t    cap;
    lv_obj_t   *box;
    Option      options[kMaxOptions];
    int         count;
};

static lv_obj_t *details_scr;
static lv_obj_t *details_title;
static lv_obj_t *details_body;
static lv_obj_t *power_btn;
static int details_id = -1;
static bool details_on = false;
static SectionUi sections[] = {
    {"SOURCES", kCapSourceActivation, nullptr, {}, 0},
    {"RADIO", kCapRadio, nullptr, {}, 0},
    {"GROUP", kCapMultiroom, nullptr, {}, 0},
};

static void on_back(lv_event_t *) { nav::to_devices(); }

static void on_details_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) nav::to_devices();
}

static void on_power(lv_event_t *) {
    details_on = !details_on;
    widgets::set_active(power_btn, details_on);
    commands::post(commands::Type::Power, details_id, details_on);
}

static void on_option(lv_event_t *e) {
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    int s = packed >> 8, i = packed & 0xFF;
    SectionUi &sec = sections[s];
    switch ((Section)s) {
        case Section::Sources:
            commands::post(commands::Type::ActivateSource, details_id, 0, sec.options[i].id);
            break;
        case Section::Radio:
            commands::post(commands::Type::PlayRadio, details_id, atoi(sec.options[i].id));
            break;
        case Section::Group:
            if (i == sec.count) commands::post(commands::Type::LeaveGroup, details_id);
            else commands::post(commands::Type::JoinGroup, details_id, atoi(sec.options[i].id));
            break;
    }
}

static void add_option_row(int s, int i, const char *title, const char *detail, bool active) {
    lv_obj_t *t, *d;
    lv_obj_t *r = list_row(sections[s].box, on_option, (void *)(intptr_t)(s << 8 | i), &t, detail && *detail ? &d : nullptr);
    lv_label_set_text(t, title);
    if (detail && *detail) lv_label_set_text(d, detail);
    if (active) {
        lv_obj_t *check = widgets::label(r, LV_SYMBOL_OK, layout.font_body, color::text());
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -layout.gap, 0);
    }
}

void set_options(int device_id, Section section, const Option *options, int count) {
    if (device_id != details_id) return;
    int s = (int)section;
    SectionUi &sec = sections[s];
    if (!sec.box) return;
    lv_obj_clean(sec.box);

    sec.count = LV_MIN(count, kMaxOptions);
    memcpy(sec.options, options, sizeof(Option) * sec.count);
    for (int i = 0; i < sec.count; i++) {
        char title[64];
        if (section == Section::Group) snprintf(title, sizeof(title), "Join %s", sec.options[i].label);
        else strlcpy(title, sec.options[i].label, sizeof(title));
        add_option_row(s, i, title, sec.options[i].detail, sec.options[i].active);
    }
    if (section == Section::Group) add_option_row(s, sec.count, "Leave group", nullptr, false);
    else if (sec.count == 0) widgets::label(sec.box, "Nothing available", layout.font_small, color::muted());
}

void open_details(int device_id) {
    int index = -1;
    for (int i = 0; i < row_count; i++)
        if (rows[i].id == device_id) index = i;
    if (index < 0) return;
    const Row &r = rows[index];

    details_id = device_id;
    lv_label_set_text(details_title, lv_label_get_text(r.name));
    details_on = r.state == PlayState::Playing || r.state == PlayState::Paused;
    widgets::set_active(power_btn, details_on);
    if (r.caps & kCapPower) lv_obj_clear_flag(power_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(power_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(details_body);
    bool any = false;
    for (SectionUi &sec : sections) {
        sec.box   = nullptr;
        sec.count = 0;
        if (!(r.caps & sec.cap)) continue;
        any = true;
        lv_obj_t *h = widgets::label(details_body, sec.title, layout.font_small, color::muted());
        lv_obj_set_style_pad_top(h, layout.margin, 0);
        sec.box = widgets::box(details_body);
        lv_obj_set_size(sec.box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sec.box, LV_FLEX_FLOW_COLUMN);
        widgets::label(sec.box, "Loading...", layout.font_small, color::muted());
    }
    if (!any) widgets::label(details_body, "Nothing to set for this device", layout.font_body, color::muted());
    else commands::post(commands::Type::LoadDetails, device_id);
}

// ---------------------------------------------------------------------------------

void build(const Layout &l) {
    layout = l;

    list_scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(list_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(list_scr, color::bg(), 0);
    widgets::on_gesture(list_scr, on_list_gesture);
    header(list_scr, "Devices", LV_SYMBOL_DOWN, on_close);
    list_box = scroll_column(list_scr);

    details_scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(details_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(details_scr, color::bg(), 0);
    widgets::on_gesture(details_scr, on_details_gesture);
    lv_obj_t *h = header(details_scr, "", LV_SYMBOL_LEFT, on_back, &details_title);
    // Back on the left, like a navigation bar; power on the right
    lv_obj_t *back = lv_obj_get_child(h, 1);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(details_title, LV_ALIGN_LEFT_MID, layout.btn_small + layout.gap, 0);
    power_btn = widgets::round_button(h, LV_SYMBOL_POWER, layout.btn_small, layout.font_body, on_power);
    lv_obj_align(power_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    details_body = scroll_column(details_scr);
}

lv_obj_t *list_screen() { return list_scr; }
lv_obj_t *details_screen() { return details_scr; }

}  // namespace ui::devices
