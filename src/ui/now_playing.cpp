#include "now_playing.h"

#include <Arduino.h>

#include "app_settings.h"
#include "nav.h"
#include "net/commands.h"
#include "ui.h"
#include "widgets.h"

namespace ui::now_playing {

static const uint32_t kCoverFadeMs       = 400;
static const uint32_t kBackgroundFadeMs  = 700;
// Calm: while playing and untouched (settings::calm_after_s), the controls fade out, the bar
// fades, the background dims and cover + text slide to the centre; the next touch brings
// them back (that touch only wakes, the buttons are hidden)
static const uint32_t kCalmFadeMs  = 600;
static const lv_opa_t kCalmDim     = LV_OPA_40;  // black over the background
static const lv_opa_t kCalmBar     = LV_OPA_40;  // progress bar and times

// Two stacked images per artwork kind: `front` is visible, the other fades in on change
struct Crossfade {
    lv_obj_t  *img[2];
    int        front = 0;
    lv_coord_t target;  // displayed size (covers are scaled to fit), 0 = native
};

// What this screen shows of the current device; updated optimistically on taps
static struct {
    int       id = -1;
    uint16_t  caps = 0;
    PlayState state = PlayState::Unknown;
    int8_t    shuffle = -1;
    Repeat    repeat = Repeat::Unknown;
    int8_t    liked = -1;
    int       album_id = 0;   // library album, 0 = none (tapping the cover does nothing)
    int       duration = -1;  // seconds
    int       elapsed  = 0;   // seconds, ticks on locally while playing
} shown;

static Layout layout;
static lv_obj_t *scr;
static Crossfade background;
static Crossfade cover;
static lv_obj_t *cover_box;
static lv_obj_t *track_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *elapsed_label;   // landscape only
static lv_obj_t *duration_label;  // landscape only
static lv_obj_t *device_label;
static lv_obj_t *notice_label;
static lv_obj_t *progress_bar;
static lv_obj_t *controls;     // transport + modes (+ device name in landscape)
static lv_obj_t *times_row;    // landscape only
static lv_obj_t *dim_layer;    // black between background and content, see kCalmDim
static lv_obj_t *info_box;     // track, artist, album (+ bar and times in landscape)
static bool      calm = false;
static lv_obj_t *transport_row;
static lv_obj_t *play_btn;
static lv_obj_t *modes_row;
static lv_obj_t *shuffle_btn;
static lv_obj_t *repeat_btn;
static lv_obj_t *like_btn;

// ---- Times (landscape) -------------------------------------------------------------

static void format_time(char *buf, size_t size, int seconds) {
    snprintf(buf, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void show_times() {
    if (!elapsed_label) return;
    if (shown.duration <= 0) {
        lv_label_set_text(elapsed_label, "");
        lv_label_set_text(duration_label, "");
        return;
    }
    char buf[12];
    format_time(buf, sizeof(buf), LV_MIN(shown.elapsed, shown.duration));
    lv_label_set_text(elapsed_label, buf);
    format_time(buf, sizeof(buf), shown.duration);
    lv_label_set_text(duration_label, buf);
}

// With a known duration the bar steps once a second (about 2 px on the 7"), which reads as
// steady motion. No animation: continuous redraws brought the RGB panel glitches back.
// Without a duration it shows the server's percentage.
static void show_bar(bool animate) {
    // Range only when it changes: setting it redraws the whole bar, not just the new pixels
    static int range = -1;
    int wanted = shown.duration > 0 ? shown.duration : 100;
    if (wanted != range) {
        lv_bar_set_range(progress_bar, 0, wanted);
        range = wanted;
    }
    if (shown.duration > 0) {
        lv_bar_set_value(progress_bar, LV_MIN(shown.elapsed, shown.duration), animate ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

static void on_gesture(lv_dir_t dir);

// /progress is a percentage (1% of a 4 minute track is 2.4 s): count seconds locally
static bool seeking = false;  // a finger is on the bar: it shows where it would seek to
static bool seek_cancelled = false;  // the finger went up/down instead of along: a swipe, not a seek
static lv_point_t seek_origin;
static int        seek_start_elapsed;

static void tick_elapsed(lv_timer_t *) {
    if (seeking || shown.state != PlayState::Playing || shown.duration <= 0 || shown.elapsed >= shown.duration) return;
    shown.elapsed++;
    show_times();
    show_bar(false);
}

// Drag on the bar to seek: the bar and time follow the finger, and the seek is sent once on
// release. In calm mode (bar faded) a touch only wakes.
static void on_bar(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESS_LOST) {
        seeking = false;
        return;
    }
    if (calm || shown.duration <= 0 || !(shown.caps & kCapSeek)) return;

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        if (code == LV_EVENT_PRESSED) {
            seek_cancelled     = false;
            seek_origin        = p;
            seek_start_elapsed = shown.elapsed;
        }
        // A swipe up to the devices starts on the bar too: once the finger moves mostly vertically,
        // put the position back and leave the gesture to the screen
        if (!seek_cancelled && LV_ABS(p.y - seek_origin.y) > layout.btn / 2 &&
            LV_ABS(p.y - seek_origin.y) > LV_ABS(p.x - seek_origin.x)) {
            seek_cancelled = true;
            seeking        = false;
            shown.elapsed  = seek_start_elapsed;
            show_times();
            show_bar(false);
            // The bar doesn't pass gestures on (a drag along it is a seek), so hand this one over
            on_gesture(p.y < seek_origin.y ? LV_DIR_TOP : LV_DIR_BOTTOM);
        }
        if (seek_cancelled) return;
        lv_area_t a;
        lv_obj_get_coords(progress_bar, &a);
        int w   = LV_MAX(lv_area_get_width(&a), 1);
        int x   = LV_CLAMP(0, p.x - a.x1, w);
        seeking = true;
        shown.elapsed = (int)((int64_t)x * shown.duration / w);
        show_times();
        show_bar(false);
    } else if (code == LV_EVENT_RELEASED && seeking && !seek_cancelled) {
        seeking = false;
        commands::post(commands::Type::Seek, shown.id, shown.elapsed);
    }
}

// ---- Controls -------------------------------------------------------------------

static void show_play_state() {
    widgets::set_symbol(play_btn, shown.state == PlayState::Playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void show_modes() {
    widgets::set_active(shuffle_btn, shown.shuffle == 1);
    widgets::set_active(repeat_btn, shown.repeat == Repeat::All || shown.repeat == Repeat::One);
    widgets::set_symbol(repeat_btn, shown.repeat == Repeat::One ? LV_SYMBOL_LOOP "1" : LV_SYMBOL_LOOP);
    widgets::set_active(like_btn, shown.liked == 1);
    widgets::set_symbol(like_btn, shown.liked == 1 ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
}

static void on_play(lv_event_t *) {
    if (shown.id < 0) return;
    bool playing = shown.state == PlayState::Playing;
    commands::post(playing ? commands::Type::Pause : commands::Type::Play, shown.id);
    shown.state = playing ? PlayState::Paused : PlayState::Playing;
    show_play_state();
}

static void on_previous(lv_event_t *) {
    if (shown.id >= 0) commands::post(commands::Type::Previous, shown.id);
}

static void on_next(lv_event_t *) {
    if (shown.id >= 0) commands::post(commands::Type::Next, shown.id);
}

static void on_shuffle(lv_event_t *) {
    if (shown.id < 0) return;
    shown.shuffle = shown.shuffle == 1 ? 0 : 1;
    commands::post(commands::Type::Shuffle, shown.id, shown.shuffle);
    show_modes();
}

static void on_repeat(lv_event_t *) {
    if (shown.id < 0) return;
    // off -> all -> one -> off
    shown.repeat = shown.repeat == Repeat::All ? Repeat::One : shown.repeat == Repeat::One ? Repeat::Off : Repeat::All;
    commands::post(commands::Type::Repeat, shown.id, (int)shown.repeat);
    show_modes();
}

static void on_like(lv_event_t *) {
    if (shown.id < 0) return;
    shown.liked = shown.liked == 1 ? 0 : 1;
    commands::post(commands::Type::Like, shown.id, shown.liked);
    show_modes();
}

static void set_visible(lv_obj_t *obj, bool visible) {
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show_capabilities() {
    bool media = (shown.caps & kCapMediaControls) != 0;
    set_visible(transport_row, media);
    // A known mode value also counts as support: capabilities only refresh every few
    // minutes, while e.g. Spotify routing to this speaker brings its modes along at once
    bool shuffle = (shown.caps & kCapShuffle) || shown.shuffle >= 0;
    bool repeat  = (shown.caps & kCapRepeat) || shown.repeat != Repeat::Unknown;
    bool like    = (shown.caps & kCapLike) || shown.liked >= 0;
    set_visible(shuffle_btn, shuffle);
    set_visible(repeat_btn, repeat);
    set_visible(like_btn, like);
    set_visible(modes_row, shuffle || repeat || like);
}

// ---- Gestures: swipe up = devices, swipe down = library, swipe right = volume,
// swipe left = queue ---------

static void on_gesture(lv_dir_t dir) {
    switch (dir) {
        case LV_DIR_TOP:    nav::to_devices(); break;
        case LV_DIR_BOTTOM: nav::to_library(); break;
        case LV_DIR_RIGHT:  nav::to_volume(); break;
        case LV_DIR_LEFT:   nav::to_queue(); break;
        default: break;
    }
}

// Tap the cover: open the playing album in the library. In calm mode a touch only wakes.
static void on_cover(lv_event_t *) {
    if (calm || shown.album_id <= 0) return;
    nav::to_album(shown.album_id);
}

// ---- Cross-fade -----------------------------------------------------------------

static void set_img_opa(void *obj, int32_t v) { lv_obj_set_style_img_opa((lv_obj_t *)obj, v, 0); }

static void on_faded_out(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

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

static void crossfade_to(Crossfade &cf, const lv_img_dsc_t *image, uint32_t ms) {
    lv_obj_t *old_front = cf.img[cf.front];
    if (!image) {
        fade(old_front, false, ms);
        return;
    }
    lv_obj_t *next = cf.img[1 - cf.front];
    lv_img_cache_invalidate_src(image);
    lv_img_set_src(next, image);
    if (cf.target > 0 && image->header.w > 0) {
        lv_img_set_zoom(next, (uint32_t)cf.target * LV_IMG_ZOOM_NONE / image->header.w);
    }
    lv_obj_set_style_img_opa(next, LV_OPA_TRANSP, 0);
    // Only above its sibling layer: the background layers must stay behind the rest of the screen
    if (lv_obj_get_index(next) < lv_obj_get_index(old_front)) lv_obj_swap(next, old_front);
    fade(next, true, ms);
    fade(old_front, false, ms);
    cf.front = 1 - cf.front;
}

static lv_obj_t *make_img(lv_obj_t *parent) {
    lv_obj_t *img = lv_img_create(parent);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_img_opa(img, LV_OPA_TRANSP, 0);
    return img;
}

// ---- Calm ---------------------------------------------------------------------------

static void set_opa(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); }
static void set_bg_opa(void *obj, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)obj, v, 0); }
static void set_translate_y(void *obj, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0); }

static void animate(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, lv_anim_ready_cb_t ready = nullptr) {
    if (!obj) return;
    lv_anim_del(obj, exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, kCalmFadeMs);
    if (ready) lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

static void fade_obj(lv_obj_t *obj, lv_opa_t to, bool hide_after = false) {
    if (!obj) return;
    if (to > 0) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    animate(obj, set_opa, lv_obj_get_style_opa(obj, 0), to, hide_after && to == 0 ? on_faded_out : nullptr);
}

static void set_calm(bool on) {
    if (on == calm) return;
    calm = on;
    fade_obj(controls, on ? LV_OPA_TRANSP : LV_OPA_COVER, true);
    if (!layout.landscape) fade_obj(device_label, on ? LV_OPA_TRANSP : LV_OPA_COVER, true);
    fade_obj(progress_bar, on ? kCalmBar : LV_OPA_COVER);
    fade_obj(times_row, on ? kCalmBar : LV_OPA_COVER);
    animate(dim_layer, set_bg_opa, lv_obj_get_style_bg_opa(dim_layer, 0), on ? kCalmDim : LV_OPA_TRANSP);

    // Cover + text centred vertically in the room the controls leave: the whole height in
    // landscape, above the progress bar in portrait
    lv_coord_t dy = 0;
    if (on) {
        lv_obj_update_layout(scr);
        lv_area_t c, i;
        lv_obj_get_coords(cover_box, &c);
        lv_obj_get_coords(info_box, &i);
        lv_coord_t shift  = lv_obj_get_style_translate_y(cover_box, 0);  // mid-animation
        lv_coord_t top    = LV_MIN(c.y1, i.y1) - shift;
        lv_coord_t height = LV_MAX(c.y2, i.y2) - LV_MIN(c.y1, i.y1) + 1;
        lv_coord_t bottom = layout.landscape ? layout.height : progress_bar->coords.y1 - layout.gap;
        dy = (bottom - height) / 2 - top;
    }
    animate(cover_box, set_translate_y, lv_obj_get_style_translate_y(cover_box, 0), dy);
    animate(info_box, set_translate_y, lv_obj_get_style_translate_y(info_box, 0), dy);
}

static void check_calm(lv_timer_t *) {
    int after = settings::calm_after_s();
    bool want = after > 0 && lv_scr_act() == scr && shown.state == PlayState::Playing &&
                lv_disp_get_inactive_time(nullptr) >= (uint32_t)after * 1000;
    set_calm(want);
}

// ---------------------------------------------------------------------------------

static lv_obj_t *row(lv_obj_t *parent, lv_coord_t gap) {
    lv_obj_t *r = widgets::box(parent);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

void build(const Layout &l) {
    layout = l;
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    widgets::on_gesture(scr, on_gesture);

    // Server-rendered background, centered behind everything
    for (lv_obj_t *&img : background.img) {
        img = make_img(scr);
        lv_obj_center(img);
    }
    background.target = 0;
    dim_layer = widgets::box(scr);
    lv_obj_set_size(dim_layer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(dim_layer, LV_OBJ_FLAG_CLICKABLE);

    // Cover: a plain square placeholder, with the two artwork layers on top
    cover_box = widgets::box(scr);
    lv_obj_set_size(cover_box, l.cover, l.cover);
    lv_obj_add_flag(cover_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cover_box, LV_OBJ_FLAG_GESTURE_BUBBLE);  // swipes still reach the screen
    lv_obj_add_event_cb(cover_box, on_cover, LV_EVENT_CLICKED, nullptr);
    // Square on purpose: clip_corner renders the cover through a masked layer in PSRAM, and on
    // the 7" RGB panel that extra traffic brought the glitches back
    lv_obj_t *placeholder = widgets::box(cover_box);
    lv_obj_set_size(placeholder, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_center(widgets::label(placeholder, LV_SYMBOL_AUDIO, l.font_title, lv_color_hex(0x505050)));
    for (lv_obj_t *&img : cover.img) {
        img = make_img(cover_box);
        lv_img_set_pivot(img, 0, 0);
        lv_obj_set_pos(img, 0, 0);
    }
    cover.target = l.cover;

    // Text: right of the cover (landscape, like the reference design) or below it (portrait)
    lv_obj_t *info = info_box = widgets::box(scr);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, l.gap / 2, 0);
    lv_obj_set_height(info, LV_SIZE_CONTENT);

    // Landscape: large title, artist and album in grey below it
    track_label = widgets::label(info, "", l.landscape ? l.font_code : l.font_title, color::text());
    lv_obj_set_width(track_label, lv_pct(100));
    lv_label_set_long_mode(track_label, LV_LABEL_LONG_WRAP);

    artist_label = widgets::label(info, "", l.landscape ? l.font_title : l.font_body, color::muted());
    lv_obj_set_width(artist_label, lv_pct(100));
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);

    album_label = widgets::label(info, "", l.landscape ? l.font_title : l.font_body, color::muted());
    lv_obj_set_width(album_label, lv_pct(100));
    lv_label_set_long_mode(album_label, LV_LABEL_LONG_DOT);
    if (!l.landscape) lv_obj_add_flag(album_label, LV_OBJ_FLAG_HIDDEN);  // no room in portrait

    // A bar you can drag (on_bar), not a slider: the look stays, and it swallows no swipes
    // outside its own area. Landscape: in the column, with elapsed / total time below. Portrait: bottom, full width.
    lv_obj_t *bar_parent = info;
    if (l.landscape) {
        lv_obj_t *spacer = widgets::box(info);  // LVGL 8 has no margins
        lv_obj_set_size(spacer, 1, l.gap);
    } else {
        bar_parent = scr;
    }
    progress_bar = lv_bar_create(bar_parent);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(progress_bar, 0, LV_PART_MAIN);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(progress_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);  // a drag along it is a seek, not a swipe
    lv_obj_set_ext_click_area(progress_bar, l.btn / 3);           // 8-10 px is too thin to hit
    lv_obj_add_event_cb(progress_bar, on_bar, LV_EVENT_ALL, nullptr);
    if (l.landscape) {
        lv_obj_set_size(progress_bar, lv_pct(100), 10);
        lv_obj_t *times = times_row = widgets::box(info);
        lv_obj_set_size(times, lv_pct(100), LV_SIZE_CONTENT);
        elapsed_label = widgets::label(times, "", l.font_body, color::muted());
        lv_obj_align(elapsed_label, LV_ALIGN_LEFT_MID, 0, 0);
        duration_label = widgets::label(times, "", l.font_body, color::muted());
        lv_obj_align(duration_label, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_set_size(progress_bar, l.width - 2 * l.margin, 8);
        lv_obj_align(progress_bar, LV_ALIGN_BOTTOM_MID, 0, -l.margin);
    }
    set_accent(color::accent());

    // Controls. Landscape: a bottom row (device name left, transport centred, modes right).
    // Portrait: anchored above the progress bar, so a long title can't push them into it.
    if (l.landscape) {
        controls = widgets::box(scr);
        lv_obj_set_size(controls, l.width - 2 * l.margin, l.btn_large);
        lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -l.margin);
    } else {
        controls = widgets::box(scr);
        lv_obj_set_size(controls, l.width - 2 * l.margin, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -(l.margin + 10 + l.gap));
    }

    transport_row = row(controls, l.margin);
    widgets::round_button(transport_row, LV_SYMBOL_PREV, l.btn, l.font_body, on_previous);
    play_btn = widgets::round_button(transport_row, LV_SYMBOL_PLAY, l.btn_large, l.font_title, on_play);
    widgets::round_button(transport_row, LV_SYMBOL_NEXT, l.btn, l.font_body, on_next);

    modes_row = row(controls, l.gap);
    shuffle_btn = widgets::round_button(modes_row, LV_SYMBOL_SHUFFLE, l.btn_small, l.font_small, on_shuffle);
    repeat_btn  = widgets::round_button(modes_row, LV_SYMBOL_LOOP, l.btn_small, l.font_small, on_repeat);
    like_btn    = widgets::round_button(modes_row, LV_SYMBOL_PLUS, l.btn_small, l.font_small, on_like);

    if (l.landscape) {
        lv_obj_align(transport_row, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(modes_row, LV_ALIGN_RIGHT_MID, 0, 0);
        device_label = widgets::label(controls, "", l.font_title, color::muted());
        lv_obj_set_width(device_label, (l.width - 2 * l.margin) / 2 - l.btn_large - l.btn - l.margin);
        lv_label_set_long_mode(device_label, LV_LABEL_LONG_DOT);
        lv_obj_align(device_label, LV_ALIGN_LEFT_MID, 0, 0);

        // Cover and text centred as one group in the space above the controls
        lv_coord_t above = l.height - l.btn_large - 2 * l.margin;
        lv_obj_align(cover_box, LV_ALIGN_TOP_LEFT, l.margin * 2, (above - l.cover) / 2 + l.margin / 2);
        lv_coord_t x = l.margin * 2 + l.cover + l.margin * 2;
        lv_obj_set_width(info, l.width - x - l.margin * 2);
        lv_obj_align_to(info, cover_box, LV_ALIGN_OUT_RIGHT_MID, l.margin * 2, 0);
    } else {
        lv_obj_set_style_pad_top(modes_row, l.gap, 0);
        lv_obj_align(cover_box, LV_ALIGN_TOP_MID, 0, l.margin);
        lv_obj_set_width(info, l.width - 2 * l.margin);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align_to(info, cover_box, LV_ALIGN_OUT_BOTTOM_MID, 0, l.gap);
        lv_label_set_long_mode(track_label, LV_LABEL_LONG_SCROLL_CIRCULAR);  // one line in portrait
        lv_obj_set_style_text_align(track_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
        device_label = widgets::label(scr, "", l.font_small, color::muted());
        lv_obj_align(device_label, LV_ALIGN_TOP_RIGHT, -l.gap, l.gap);
    }

    notice_label = widgets::label(scr, "", l.font_small, color::muted());
    if (l.landscape) lv_obj_align(notice_label, LV_ALIGN_TOP_RIGHT, -l.margin, l.gap);
    else lv_obj_align_to(notice_label, progress_bar, LV_ALIGN_OUT_TOP_LEFT, 0, -l.gap);
    lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);

    // Elapsed time runs on between the server's progress updates
    lv_timer_create(tick_elapsed, 1000, nullptr);
    lv_timer_create(check_calm, 250, nullptr);

    show_capabilities();
    show_play_state();
    show_modes();
    lv_label_set_text(track_label, "-");
    lv_obj_set_style_opa(cover_box, LV_OPA_50, 0);
}

lv_obj_t *screen() { return scr; }

void update(const Device &d, uint8_t what) {
    if (d.id != shown.id) {
        shown.id = d.id;
        what     = kUpdateAll;
    }

    if (what & kUpdateDevice) {
        lv_label_set_text(device_label, d.name);
        shown.caps = d.caps;
        show_capabilities();
    }
    if (what & (kUpdateTrack | kUpdateState)) {
        bool active = d.active();
        shown.album_id = active ? d.album_id : 0;
        lv_label_set_text(track_label, active && d.track[0] ? d.track : "-");
        lv_label_set_text(artist_label, active ? d.artist : "");
        lv_label_set_text(album_label, active ? d.album : "");
        if (shown.duration != (active ? d.duration : -1)) shown.elapsed = d.progress * LV_MAX(d.duration, 0) / 100;
        shown.duration = active ? d.duration : -1;
        if (!active) shown.elapsed = 0;
        show_times();
        show_bar(false);
        // Dim the cover while nothing plays
        lv_obj_set_style_opa(cover_box, active ? LV_OPA_COVER : LV_OPA_50, 0);
        if (!active) lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (what & kUpdateState) {
        shown.state = d.state;
        show_play_state();
    }
    if ((what & kUpdateProgress) && !seeking) {
        // Resync the local clock only when it drifted further than the percentage's resolution
        if (shown.duration > 0) {
            int from_server = d.progress * shown.duration / 100;
            bool jump = abs(from_server - shown.elapsed) > shown.duration / 100 + 1;
            if (jump) shown.elapsed = from_server;
            show_times();
            if (jump) show_bar(false);
        } else {
            lv_bar_set_value(progress_bar, d.progress, LV_ANIM_OFF);
        }
    }
    if (what & kUpdateModes) {
        shown.shuffle = d.shuffle;
        shown.repeat  = d.repeat;
        shown.liked   = d.liked;
        show_modes();
        show_capabilities();
    }
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
