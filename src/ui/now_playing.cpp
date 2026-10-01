#include "now_playing.h"

#include <Arduino.h>

namespace ui::now_playing {

static const uint32_t kCoverFadeMs      = 400;
static const uint32_t kBackgroundFadeMs = 700;

// Two stacked images per artwork kind: `front` is visible, the other fades in on change
struct Crossfade {
    lv_obj_t *img[2];
    int       front = 0;
    lv_coord_t target;  // displayed size (covers are scaled to fit), 0 = native
};

static lv_obj_t *scr;
static Crossfade background;
static Crossfade cover;
static lv_obj_t *cover_box;
static lv_obj_t *placeholder;
static lv_obj_t *track_label;
static lv_obj_t *artist_label;
static lv_obj_t *device_label;
static lv_obj_t *notice_label;
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

// ---- Cross-fade -----------------------------------------------------------------

static void set_img_opa(void *obj, int32_t v) { lv_obj_set_style_img_opa((lv_obj_t *)obj, v, 0); }

static void on_faded_out(lv_anim_t *a) {
    lv_obj_t *img = (lv_obj_t *)a->var;
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
}

static void fade(lv_obj_t *img, bool in, uint32_t ms) {
    lv_anim_del(img, set_img_opa);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, set_img_opa);
    lv_anim_set_values(&a, lv_obj_get_style_img_opa(img, 0), in ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_anim_set_time(&a, ms);
    if (!in) lv_anim_set_ready_cb(&a, on_faded_out);
    lv_anim_start(&a);
}

// Shows `image` with a cross-fade. Returns whether anything is visible afterwards.
static bool crossfade_to(Crossfade &cf, const lv_img_dsc_t *image, uint32_t ms) {
    lv_obj_t *old_front = cf.img[cf.front];
    if (!image) {
        fade(old_front, false, ms);
        return false;
    }
    lv_obj_t *next = cf.img[1 - cf.front];
    lv_img_cache_invalidate_src(image);
    lv_img_set_src(next, image);
    if (cf.target > 0 && image->header.w > 0) {
        lv_img_set_zoom(next, (uint32_t)cf.target * LV_IMG_ZOOM_NONE / image->header.w);
    }
    lv_obj_set_style_img_opa(next, LV_OPA_TRANSP, 0);
    lv_obj_move_foreground(next);
    fade(next, true, ms);
    fade(old_front, false, ms);
    cf.front = 1 - cf.front;
    return true;
}

static lv_obj_t *make_img(lv_obj_t *parent) {
    lv_obj_t *img = lv_img_create(parent);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_img_opa(img, LV_OPA_TRANSP, 0);
    return img;
}

// ---------------------------------------------------------------------------------

void build(const Layout &l) {
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, nullptr);

    // Server-rendered background, centered behind everything
    for (lv_obj_t *&img : background.img) {
        img = make_img(scr);
        lv_obj_center(img);
    }
    background.target = 0;

    // Cover: a plain square placeholder, with the two artwork layers on top
    cover_box = lv_obj_create(scr);
    lv_obj_remove_style_all(cover_box);
    lv_obj_clear_flag(cover_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cover_box, l.cover, l.cover);

    placeholder = lv_obj_create(cover_box);
    lv_obj_remove_style_all(placeholder);
    lv_obj_set_size(placeholder, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_t *note = lv_label_create(placeholder);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, l.font_title, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0x505050), 0);
    lv_obj_center(note);

    for (lv_obj_t *&img : cover.img) {
        img = make_img(cover_box);
        lv_img_set_pivot(img, 0, 0);
        lv_obj_set_pos(img, 0, 0);
    }
    cover.target = l.cover;

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
        lv_obj_set_pos(cover_box, l.margin, l.margin);
        lv_coord_t x = 2 * l.margin + l.cover;
        lv_obj_set_pos(text, x, l.margin + l.gap);
        lv_obj_set_width(text, l.width - x - l.margin);
    } else {
        lv_obj_align(cover_box, LV_ALIGN_TOP_MID, 0, l.margin);
        lv_obj_set_width(text, l.width - 2 * l.margin);
        lv_obj_align_to(text, cover_box, LV_ALIGN_OUT_BOTTOM_MID, 0, l.margin);
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
    set_accent(color::accent());

    notice_label = lv_label_create(scr);
    lv_obj_set_style_text_font(notice_label, l.font_small, 0);
    lv_obj_set_style_text_color(notice_label, color::muted(), 0);
    lv_obj_align_to(notice_label, progress_bar, LV_ALIGN_OUT_TOP_LEFT, 0, -l.gap);
    lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);

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
    // Dim everything artwork-related while nothing plays
    lv_obj_set_style_opa(cover_box, playing ? LV_OPA_COVER : LV_OPA_50, 0);
    if (playing) return;
    lv_label_set_text(track_label, "—");  // em dash
    lv_label_set_text(artist_label, "");
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
}

void set_notice(const char *text) {
    if (text && *text) {
        lv_label_set_text(notice_label, text);
        lv_obj_clear_flag(notice_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_cover(const lv_img_dsc_t *image) { crossfade_to(cover, image, kCoverFadeMs); }

void set_background(const lv_img_dsc_t *image) { crossfade_to(background, image, kBackgroundFadeMs); }

void set_accent(lv_color_t c) { lv_obj_set_style_bg_color(progress_bar, c, LV_PART_INDICATOR); }

void release_image(const lv_img_dsc_t *image) {
    for (Crossfade *cf : {&cover, &background}) {
        for (lv_obj_t *img : cf->img) {
            if (lv_img_get_src(img) != image) continue;
            lv_anim_del(img, set_img_opa);
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_img_opa(img, LV_OPA_TRANSP, 0);
            lv_img_set_src(img, nullptr);
        }
    }
    lv_img_cache_invalidate_src(image);
}

}  // namespace ui::now_playing
