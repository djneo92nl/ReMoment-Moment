#include "ui.h"

#include "layout.h"
#include "now_playing.h"

namespace ui {

static Layout layout;

static lv_obj_t *status_screen;
static lv_obj_t *status_message;
static lv_obj_t *status_detail;
static lv_obj_t *status_code;

static lv_obj_t *connection_icon;

// ---- Status screen (startup, setup portal, pairing) --------------------------

static void build_status_screen() {
    status_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_screen, color::bg(), 0);
    lv_obj_set_flex_flow(status_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(status_screen, layout.gap, 0);
    lv_obj_set_style_pad_all(status_screen, layout.margin, 0);

    lv_obj_t *logo = lv_label_create(status_screen);
    lv_label_set_text(logo, "ReMoment");
    lv_obj_set_style_text_font(logo, layout.font_code, 0);
    lv_obj_set_style_text_color(logo, color::text(), 0);
    lv_obj_set_style_pad_bottom(logo, layout.margin, 0);

    status_message = lv_label_create(status_screen);
    lv_obj_set_style_text_font(status_message, layout.font_body, 0);
    lv_obj_set_style_text_color(status_message, color::text(), 0);

    status_code = lv_label_create(status_screen);
    lv_obj_set_style_text_font(status_code, layout.font_code, 0);
    lv_obj_set_style_text_color(status_code, color::text(), 0);
    lv_obj_set_style_text_letter_space(status_code, 8, 0);

    status_detail = lv_label_create(status_screen);
    lv_obj_set_width(status_detail, layout.width - 2 * layout.margin);
    lv_label_set_long_mode(status_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_detail, layout.font_small, 0);
    lv_obj_set_style_text_color(status_detail, color::muted(), 0);
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
    if (!lock) return;
    lv_obj_t *screen = now_playing::screen();
    if (lv_scr_act() != screen) lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
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

// ---- Now playing (forwarders, so callers only need ui.h) -----------------------

void now_playing_set_device(const char *name) {
    LvglLock lock;
    if (lock) now_playing::set_device(name);
}

void now_playing_set_track(const char *artist, const char *track) {
    LvglLock lock;
    if (lock) now_playing::set_track(artist, track);
}

void now_playing_set_progress(int percent) {
    LvglLock lock;
    if (lock) now_playing::set_progress(percent);
}

void now_playing_set_playing(bool playing) {
    LvglLock lock;
    if (lock) now_playing::set_playing(playing);
}

void now_playing_set_notice(const char *text) {
    LvglLock lock;
    if (lock) now_playing::set_notice(text);
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

    connection_icon = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(connection_icon, layout.font_body, 0);
    lv_obj_align(connection_icon, LV_ALIGN_TOP_LEFT, layout.gap, layout.gap);
    lv_obj_add_flag(connection_icon, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(status_message, "Starting");
    set_optional_text(status_detail, nullptr);
    set_optional_text(status_code, nullptr);
    lv_scr_load(status_screen);
}

}  // namespace ui
