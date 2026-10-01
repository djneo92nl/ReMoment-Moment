#include "now_playing.h"

#include <Arduino.h>

LV_IMG_DECLARE(img_wallpaper);
LV_IMG_DECLARE(img_album_placeholder);

namespace ui::now_playing {

static lv_obj_t *scr;
static lv_obj_t *cover;
static lv_obj_t *track_label;
static lv_obj_t *artist_label;
static lv_obj_t *device_label;
static lv_obj_t *progress_bar;
static lv_obj_t *toast;
static lv_timer_t *toast_timer;

// ---- Gestures: swipe up = devices, swipe down = library, side = volume ---------
// The target screens arrive in later phases; until then a toast confirms the gesture.

static void hide_toast(lv_timer_t *timer) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

static void show_toast(const char *text) {
    lv_label_set_text(toast, text);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

static void on_gesture(lv_event_t *) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    lv_indev_wait_release(lv_indev_get_act());
    switch (dir) {
        case LV_DIR_TOP:    show_toast("Devices - coming soon"); break;
        case LV_DIR_BOTTOM: show_toast("Library - coming soon"); break;
        case LV_DIR_LEFT:
        case LV_DIR_RIGHT:  show_toast("Volume - coming soon"); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------

void build(const Layout &l) {
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    // Temporary built-in wallpaper (Phase 2 replaces it with a server-rendered background)
    if (l.width == img_wallpaper.header.w && l.height == img_wallpaper.header.h) {
        lv_obj_set_style_bg_img_src(scr, &img_wallpaper, 0);
    }
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, nullptr);

    cover = lv_img_create(scr);
    lv_img_set_src(cover, &img_album_placeholder);
    lv_img_set_size_mode(cover, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_pivot(cover, 0, 0);
    lv_img_set_zoom(cover, (uint32_t)l.cover * LV_IMG_ZOOM_NONE / img_album_placeholder.header.w);
    lv_obj_set_size(cover, l.cover, l.cover);

    lv_obj_t *text = lv_obj_create(scr);
    lv_obj_remove_style_all(text);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, l.gap / 2, 0);
    lv_obj_set_height(text, LV_SIZE_CONTENT);

    track_label = lv_label_create(text);
    lv_obj_set_width(track_label, lv_pct(100));
    lv_label_set_long_mode(track_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(track_label, l.font_title, 0);
    lv_obj_set_style_text_color(track_label, color::text(), 0);

    artist_label = lv_label_create(text);
    lv_obj_set_width(artist_label, lv_pct(100));
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(artist_label, l.font_body, 0);
    lv_obj_set_style_text_color(artist_label, color::muted(), 0);

    if (l.landscape) {
        lv_obj_set_pos(cover, l.margin, l.margin);
        lv_coord_t x = 2 * l.margin + l.cover;
        lv_obj_set_pos(text, x, l.margin + l.gap);
        lv_obj_set_width(text, l.width - x - l.margin);
    } else {
        lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, l.margin);
        lv_obj_set_width(text, l.width - 2 * l.margin);
        lv_obj_align_to(text, cover, LV_ALIGN_OUT_BOTTOM_MID, 0, l.margin);
        lv_obj_set_style_text_align(track_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    device_label = lv_label_create(scr);
    lv_obj_set_style_text_font(device_label, l.font_small, 0);
    lv_obj_set_style_text_color(device_label, color::muted(), 0);
    lv_obj_align(device_label, LV_ALIGN_TOP_RIGHT, -l.gap, l.gap);
    lv_label_set_text(device_label, "");

    // A bar, not a slider: seeking isn't planned, and a slider would swallow swipes
    progress_bar = lv_bar_create(scr);
    lv_obj_set_size(progress_bar, l.width - 2 * l.margin, 10);
    lv_obj_align(progress_bar, LV_ALIGN_BOTTOM_MID, 0, -l.margin);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_obj_set_style_bg_color(progress_bar, color::bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(progress_bar, lv_color_hex(0x505050), LV_PART_MAIN);
    lv_obj_set_style_border_width(progress_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, color::accent(), LV_PART_INDICATOR);

    toast = lv_label_create(scr);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_pad_all(toast, l.gap, 0);
    lv_obj_set_style_text_font(toast, l.font_body, 0);
    lv_obj_set_style_text_color(toast, color::text(), 0);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = lv_timer_create(hide_toast, 1500, nullptr);
    lv_timer_set_repeat_count(toast_timer, -1);
    lv_timer_pause(toast_timer);

    set_playing(false);
}

lv_obj_t *screen() { return scr; }

void set_device(const char *name) { lv_label_set_text(device_label, name ? name : ""); }

void set_track(const char *artist, const char *track) {
    lv_label_set_text(track_label, track);
    lv_label_set_text(artist_label, artist);
}

void set_progress(int percent) { lv_bar_set_value(progress_bar, percent, LV_ANIM_ON); }

void set_playing(bool playing) {
    if (playing) {
        lv_obj_set_style_img_opa(cover, LV_OPA_COVER, 0);
        return;
    }
    lv_label_set_text(track_label, "—");  // em dash
    lv_label_set_text(artist_label, "");
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_img_opa(cover, LV_OPA_50, 0);
}

}  // namespace ui::now_playing
