#include "ui.h"

#include "devices.h"
#include "layout.h"
#include "nav.h"
#include "now_playing.h"
#include "widgets.h"

namespace ui {

static Layout layout;

static lv_obj_t *status_screen;
static lv_obj_t *status_message;
static lv_obj_t *status_detail;
static lv_obj_t *status_code;

static lv_obj_t *connection_icon;
static lv_obj_t *toast_label;
static lv_timer_t *toast_timer;

// ---- Status screen (startup, setup portal, pairing) --------------------------

static void build_status_screen() {
    status_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_screen, color::bg(), 0);
    lv_obj_set_flex_flow(status_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(status_screen, layout.gap, 0);
    lv_obj_set_style_pad_all(status_screen, layout.margin, 0);

    lv_obj_t *logo = widgets::label(status_screen, "ReMoment", layout.font_code, color::text());
    lv_obj_set_style_pad_bottom(logo, layout.margin, 0);

    status_message = widgets::label(status_screen, "", layout.font_body, color::text());

    status_code = widgets::label(status_screen, "", layout.font_code, color::text());
    lv_obj_set_style_text_letter_space(status_code, 8, 0);

    status_detail = widgets::label(status_screen, "", layout.font_small, color::muted());
    lv_obj_set_width(status_detail, layout.width - 2 * layout.margin);
    lv_label_set_long_mode(status_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_detail, LV_TEXT_ALIGN_CENTER, 0);
}

static void set_optional_text(lv_obj_t *label, const char *text) {
    if (text && *text) {
        lv_label_set_text(label, text);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

void show_status(const char *message, const char *detail, const char *code) {
    LvglLock lock;
    if (!lock) return;
    lv_label_set_text(status_message, message);
    set_optional_text(status_detail, detail);
    set_optional_text(status_code, code);
    if (lv_scr_act() != status_screen) lv_scr_load_anim(status_screen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
}

void show_now_playing() {
    LvglLock lock;
    if (lock) nav::to_now_playing();
}

// ---- Navigation (LVGL task, or callers above that hold the lock) ------------------

static void load(lv_obj_t *screen, lv_scr_load_anim_t anim) {
    if (lv_scr_act() != screen) lv_scr_load_anim(screen, anim, 250, 0, false);
}

void nav::to_now_playing() {
    lv_scr_load_anim_t anim = lv_scr_act() == devices::list_screen() ? LV_SCR_LOAD_ANIM_MOVE_BOTTOM
                                                                       : LV_SCR_LOAD_ANIM_FADE_ON;
    load(now_playing::screen(), anim);
}

void nav::to_devices() {
    lv_scr_load_anim_t anim = lv_scr_act() == devices::details_screen() ? LV_SCR_LOAD_ANIM_MOVE_RIGHT
                                                                          : LV_SCR_LOAD_ANIM_MOVE_TOP;
    load(devices::list_screen(), anim);
}

void nav::to_details(int device_id) {
    devices::open_details(device_id);
    load(devices::details_screen(), LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void hide_toast(lv_timer_t *timer) {
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

void nav::toast(const char *text) {
    lv_label_set_text(toast_label, text);
    lv_obj_clear_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast_label);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

void toast(const char *text) {
    LvglLock lock;
    if (lock) nav::toast(text);
}

// ---- Connection icon (top layer, visible on every screen) ----------------------

void set_connection(bool wifi, bool server, bool mqtt) {
    LvglLock lock;
    if (!lock) return;
    if (wifi && server && mqtt) {
        lv_obj_add_flag(connection_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(connection_icon, wifi ? LV_SYMBOL_WARNING : LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(connection_icon, wifi ? color::warning() : color::error(), 0);
    lv_obj_clear_flag(connection_icon, LV_OBJ_FLAG_HIDDEN);
}

// ---- Forwarders to the screens --------------------------------------------------

void now_playing_update(const Device &device, uint8_t what) {
    LvglLock lock;
    if (lock) now_playing::update(device, what);
}

void now_playing_set_notice(const char *text) {
    LvglLock lock;
    if (lock) now_playing::set_notice(text);
}

void devices_sync(const Device *list, uint8_t count, int current) {
    LvglLock lock;
    if (lock) devices::sync(list, count, current);
}

void devices_update(int index, const Device &device) {
    LvglLock lock;
    if (lock) devices::update(index, device);
}

void details_set_options(int device_id, Section section, const Option *options, int count) {
    LvglLock lock;
    if (lock) devices::set_options(device_id, section, options, count);
}

void artwork_show(const lv_img_dsc_t *cover, bool cover_changed, const lv_img_dsc_t *background,
                  bool background_changed, bool has_accent, uint32_t accent) {
    LvglLock lock;
    if (!lock) return;
    if (cover_changed) now_playing::set_cover(cover);
    if (background_changed) now_playing::set_background(background);
    now_playing::set_accent(has_accent ? lv_color_hex(accent) : color::accent());
}

void artwork_release(const lv_img_dsc_t *image) {
    LvglLock lock;
    if (lock) now_playing::release_image(image);
}

// ---------------------------------------------------------------------------------

void init() {
    LvglLock lock;
    if (!lock) return;

    layout = make_layout();

    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                              true, layout.font_body);
    lv_disp_set_theme(disp, theme);

    build_status_screen();
    now_playing::build(layout);
    devices::build(layout);

    connection_icon = widgets::label(lv_layer_top(), "", layout.font_body, color::error());
    lv_obj_align(connection_icon, LV_ALIGN_TOP_LEFT, layout.gap, layout.gap);
    lv_obj_add_flag(connection_icon, LV_OBJ_FLAG_HIDDEN);

    toast_label = widgets::label(lv_layer_top(), "", layout.font_body, color::text());
    lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(toast_label, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast_label, 8, 0);
    lv_obj_set_style_pad_all(toast_label, layout.gap, 0);
    lv_obj_align(toast_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    toast_timer = lv_timer_create(hide_toast, 2000, nullptr);
    lv_timer_set_repeat_count(toast_timer, -1);
    lv_timer_pause(toast_timer);

    lv_label_set_text(status_message, "Starting");
    set_optional_text(status_detail, nullptr);
    set_optional_text(status_code, nullptr);
    lv_scr_load(status_screen);
}

}  // namespace ui
