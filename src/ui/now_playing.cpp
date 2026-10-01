#include "now_playing.h"

#include <Arduino.h>

#include "nav.h"
#include "net/commands.h"
#include "ui.h"
#include "widgets.h"

namespace ui::now_playing {

static const uint32_t kCoverFadeMs       = 400;
static const uint32_t kBackgroundFadeMs  = 700;
static const uint32_t kVolumeHideMs      = 4000;
static const uint32_t kVolumeEchoGuardMs = 1500;  // ignore server echoes right after a local change

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
    int       volume = -1;
    bool      muted = false;
    int8_t    shuffle = -1;
    Repeat    repeat = Repeat::Unknown;
    int8_t    liked = -1;
} shown;

static Layout layout;
static lv_obj_t *scr;
static Crossfade background;
static Crossfade cover;
static lv_obj_t *cover_box;
static lv_obj_t *track_label;
static lv_obj_t *artist_label;
static lv_obj_t *device_label;
static lv_obj_t *notice_label;
static lv_obj_t *progress_bar;
static lv_obj_t *transport_row;
static lv_obj_t *play_btn;
static lv_obj_t *modes_row;
static lv_obj_t *shuffle_btn;
static lv_obj_t *repeat_btn;
static lv_obj_t *like_btn;

// Volume overlay (top layer)
static lv_obj_t *volume_backdrop;
static lv_obj_t *volume_panel;
static lv_obj_t *volume_label;
static lv_obj_t *volume_slider;
static lv_obj_t *mute_btn;
static lv_timer_t *volume_timer;
static uint32_t last_local_volume_ms = 0;

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

// ---- Volume overlay ---------------------------------------------------------------

static void show_volume() {
    lv_label_set_text_fmt(volume_label, shown.volume >= 0 ? "%d" : "-", shown.volume);
    if (shown.volume >= 0) lv_slider_set_value(volume_slider, shown.volume, LV_ANIM_OFF);
    widgets::set_symbol(mute_btn, shown.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    widgets::set_active(mute_btn, shown.muted);
}

static void close_volume(lv_timer_t * = nullptr) {
    lv_obj_add_flag(volume_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(volume_timer);
}

static void keep_volume_open() {
    lv_timer_reset(volume_timer);
    lv_timer_resume(volume_timer);
}

static void open_volume() {
    if (!(shown.caps & kCapVolume)) return;
    show_volume();
    lv_obj_clear_flag(volume_backdrop, LV_OBJ_FLAG_HIDDEN);
    keep_volume_open();
}

static void on_volume_changed(lv_event_t *) {
    shown.volume = lv_slider_get_value(volume_slider);
    lv_label_set_text_fmt(volume_label, "%d", shown.volume);
    last_local_volume_ms = lv_tick_get();
    commands::post(commands::Type::Volume, shown.id, shown.volume);
    keep_volume_open();
}

static void on_mute(lv_event_t *) {
    shown.muted = !shown.muted;
    last_local_volume_ms = lv_tick_get();
    commands::post(commands::Type::Mute, shown.id, shown.muted);
    show_volume();
    keep_volume_open();
}

static void on_backdrop(lv_event_t *e) {
    if (lv_event_get_target(e) == volume_backdrop) close_volume();
}

static void build_volume_overlay(const Layout &l) {
    volume_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(volume_backdrop);
    lv_obj_set_size(volume_backdrop, l.width, l.height);
    lv_obj_add_flag(volume_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(volume_backdrop, on_backdrop, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(volume_backdrop, LV_OBJ_FLAG_HIDDEN);

    volume_panel = lv_obj_create(volume_backdrop);
    lv_obj_remove_style_all(volume_panel);
    lv_obj_clear_flag(volume_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(volume_panel, l.btn * 2, l.height - 2 * l.margin);
    lv_obj_align(volume_panel, LV_ALIGN_RIGHT_MID, -l.margin, 0);
    lv_obj_set_style_radius(volume_panel, l.btn, 0);
    lv_obj_set_style_bg_color(volume_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(volume_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(volume_panel, 1, 0);
    lv_obj_set_style_border_color(volume_panel, lv_color_hex(0x303030), 0);
    lv_obj_set_style_pad_ver(volume_panel, l.margin, 0);
    lv_obj_set_flex_flow(volume_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(volume_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(volume_panel, l.margin, 0);

    volume_label = widgets::label(volume_panel, "-", l.font_title, color::text());

    volume_slider = lv_slider_create(volume_panel);
    lv_obj_set_width(volume_slider, 12);
    lv_obj_set_flex_grow(volume_slider, 1);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, color::text(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, color::text(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(volume_slider, l.btn / 2);
    lv_obj_add_event_cb(volume_slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    mute_btn = widgets::round_button(volume_panel, LV_SYMBOL_VOLUME_MAX, l.btn, l.font_body, on_mute);

    volume_timer = lv_timer_create(close_volume, kVolumeHideMs, nullptr);
    lv_timer_set_repeat_count(volume_timer, -1);
    lv_timer_pause(volume_timer);
}

// ---- Gestures: swipe up = devices, swipe down = library, side = volume ---------

static void on_gesture(lv_dir_t dir) {
    switch (dir) {
        case LV_DIR_TOP:    nav::to_devices(); break;
        case LV_DIR_BOTTOM: nav::toast("Library - coming soon"); break;
        case LV_DIR_LEFT:
        case LV_DIR_RIGHT:  open_volume(); break;
        default: break;
    }
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
    lv_obj_move_foreground(next);
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

    // Cover: a plain square placeholder, with the two artwork layers on top
    cover_box = widgets::box(scr);
    lv_obj_set_size(cover_box, l.cover, l.cover);
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

    // Text and controls: right of the cover (landscape) or below it (portrait)
    lv_obj_t *info = widgets::box(scr);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, l.gap / 2, 0);
    lv_obj_set_height(info, LV_SIZE_CONTENT);

    track_label = widgets::label(info, "", l.font_title, color::text());
    lv_obj_set_width(track_label, lv_pct(100));
    lv_label_set_long_mode(track_label, LV_LABEL_LONG_WRAP);

    artist_label = widgets::label(info, "", l.font_body, color::muted());
    lv_obj_set_width(artist_label, lv_pct(100));
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);

    transport_row = row(info, l.margin);
    lv_obj_set_style_pad_top(transport_row, l.margin, 0);
    widgets::round_button(transport_row, LV_SYMBOL_PREV, l.btn, l.font_body, on_previous);
    play_btn = widgets::round_button(transport_row, LV_SYMBOL_PLAY, l.btn_large, l.font_title, on_play);
    widgets::round_button(transport_row, LV_SYMBOL_NEXT, l.btn, l.font_body, on_next);

    modes_row = row(info, l.gap);
    lv_obj_set_style_pad_top(modes_row, l.gap, 0);
    shuffle_btn = widgets::round_button(modes_row, LV_SYMBOL_SHUFFLE, l.btn_small, l.font_small, on_shuffle);
    repeat_btn  = widgets::round_button(modes_row, LV_SYMBOL_LOOP, l.btn_small, l.font_small, on_repeat);
    like_btn    = widgets::round_button(modes_row, LV_SYMBOL_PLUS, l.btn_small, l.font_small, on_like);

    if (l.landscape) {
        lv_obj_set_pos(cover_box, l.margin, l.margin);
        lv_coord_t x = 2 * l.margin + l.cover;
        lv_obj_set_pos(info, x, l.margin + l.gap);
        lv_obj_set_width(info, l.width - x - l.margin);
    } else {
        lv_obj_align(cover_box, LV_ALIGN_TOP_MID, 0, l.margin);
        lv_obj_set_width(info, l.width - 2 * l.margin);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align_to(info, cover_box, LV_ALIGN_OUT_BOTTOM_MID, 0, l.margin);
        lv_obj_set_style_text_align(track_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    device_label = widgets::label(scr, "", l.font_small, color::muted());
    lv_obj_align(device_label, LV_ALIGN_TOP_RIGHT, -l.gap, l.gap);

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

    notice_label = widgets::label(scr, "", l.font_small, color::muted());
    lv_obj_align_to(notice_label, progress_bar, LV_ALIGN_OUT_TOP_LEFT, 0, -l.gap);
    lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);

    build_volume_overlay(l);

    show_capabilities();
    show_play_state();
    show_modes();
    lv_label_set_text(track_label, "—");  // em dash
    lv_obj_set_style_opa(cover_box, LV_OPA_50, 0);
}

lv_obj_t *screen() { return scr; }

void update(const Device &d, uint8_t what) {
    if (d.id != shown.id) {
        shown.id = d.id;
        what     = kUpdateAll;
        close_volume();
    }

    if (what & kUpdateDevice) {
        lv_label_set_text(device_label, d.name);
        shown.caps = d.caps;
        show_capabilities();
    }
    if (what & (kUpdateTrack | kUpdateState)) {
        bool active = d.active();
        lv_label_set_text(track_label, active && d.track[0] ? d.track : "—");
        lv_label_set_text(artist_label, active ? d.artist : "");
        // Dim the cover while nothing plays
        lv_obj_set_style_opa(cover_box, active ? LV_OPA_COVER : LV_OPA_50, 0);
        if (!active) lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (what & kUpdateState) {
        shown.state = d.state;
        show_play_state();
    }
    if (what & kUpdateProgress) lv_bar_set_value(progress_bar, d.progress, LV_ANIM_ON);
    if (what & kUpdateVolume) {
        // Don't let the server's echo fight the slider while it's being dragged
        bool recent_local = lv_tick_elaps(last_local_volume_ms) < kVolumeEchoGuardMs;
        if (!recent_local || what == kUpdateAll) {
            shown.volume = d.volume;
            shown.muted  = d.muted;
            show_volume();
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
