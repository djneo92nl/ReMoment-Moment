#include "queue_screen.h"

#include <Arduino.h>

#include "artwork/artwork.h"
#include "nav.h"
#include "net/commands.h"
#include "widgets.h"

namespace ui::queue_screen {

static const int kMaxRows = 20;  // the most commands.cpp loads

static Layout layout;
static lv_obj_t *scr;
static lv_obj_t *content;
static int       device_id = -1;
static uint16_t  caps      = 0;

// Covers: token = generation << 16 | seq << 12 | row. Generations from 0x8000 up, so they
// never collide with the library's (see library.cpp); each rebuild of the list bumps it,
// which also lets the artwork task free the covers of the old rows.
static uint16_t generation = 0x8000;
struct Cover {
    lv_obj_t  *img;
    lv_coord_t size;
    uint8_t    seq;
    bool       wanted;
    char       url[192];
};
static Cover covers[kMaxRows];
static int   cover_count = 0;

static void on_back(lv_event_t *) { nav::to_now_playing(); }

static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_RIGHT) nav::to_now_playing();
}

// Drops the old rows: objects first, then the artwork task may free their images
static void clear_rows() {
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    cover_count = 0;
    generation  = generation == 0xFFFF ? 0x8001 : generation + 1;
    lv_img_cache_invalidate_src(nullptr);
    artwork::thumbs_release(generation);
}

static void show_message(const char *text) {
    clear_rows();
    widgets::label(content, text, layout.font_body, color::muted());
}

// Tap a row: play it now (only where the device can pick a queue entry)
static void on_row(lv_event_t *e) {
    int position = (int)(intptr_t)lv_event_get_user_data(e);
    commands::post(commands::Type::QueueJump, device_id, position);
    nav::to_now_playing();
}

// Small cover, then the title and "artist - album", with the duration on the right
static void add_row(const QueueItem &it, int index) {
    lv_obj_t *r = widgets::box(content);
    lv_obj_set_size(r, lv_pct(100), layout.row);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
    if (caps & kCapQueueJump) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, color::text(), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)(index + 1));  // 1 = next track
    }

    lv_coord_t size = layout.row - layout.gap;
    lv_obj_t *art   = widgets::box(r);
    lv_obj_set_size(art, size, size);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_align(art, LV_ALIGN_LEFT_MID, 0, 0);
    if (it.thumb[0] && cover_count < kMaxRows) {
        lv_obj_t *img = lv_img_create(art);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(img, 0, 0);
        Cover &c = covers[cover_count++];
        c        = {img, size, 0, false, ""};
        strlcpy(c.url, it.thumb, sizeof(c.url));
    }

    char time[12] = "";
    if (it.duration > 0) snprintf(time, sizeof(time), "%d:%02d", it.duration / 60, it.duration % 60);
    lv_obj_t *d = widgets::label(r, time, layout.font_small, color::muted());
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_coord_t text_x = size + layout.gap;
    lv_coord_t text_w = layout.width - 2 * layout.margin - text_x - layout.font_small->line_height * 3 - layout.gap;

    lv_obj_t *t = widgets::label(r, it.title, layout.font_body, color::text());
    // DOT only clips to one line when the height is fixed; otherwise the label wraps
    lv_obj_set_size(t, text_w, layout.font_body->line_height);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, text_x, -layout.gap / 2 - 4);

    char sub[112];
    if (it.artist[0] && it.album[0]) snprintf(sub, sizeof(sub), "%s - %s", it.artist, it.album);
    else snprintf(sub, sizeof(sub), "%s", it.artist[0] ? it.artist : it.album);
    lv_obj_t *s = widgets::label(r, sub, layout.font_small, color::muted());
    lv_obj_set_size(s, text_w, layout.font_small->line_height);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, text_x, layout.gap / 2 + 4);
}

static uint32_t token_of(int index) {
    return (uint32_t)generation << 16 | (uint32_t)(covers[index].seq & 0xF) << 12 | index;
}

bool thumb_ready(uint32_t token, const lv_img_dsc_t *image) {
    int index = token & 0xFFF;
    if ((token >> 16) != generation || index >= cover_count) return false;
    Cover &c = covers[index];
    if (!c.wanted || c.seq != ((token >> 12) & 0xF)) return false;
    lv_img_set_src(c.img, image);
    if (image->header.w > 0) lv_img_set_zoom(c.img, (uint32_t)c.size * LV_IMG_ZOOM_NONE / image->header.w);
    return true;
}

void build(const Layout &l) {
    layout = l;
    scr    = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    widgets::on_gesture(scr, on_gesture);

    // Navigation bar: title on the left, the way back to Now Playing on the right
    lv_obj_t *h = widgets::box(scr);
    lv_obj_set_size(h, l.width - 2 * l.margin, l.btn);
    lv_obj_set_pos(h, l.margin, l.margin / 2);
    lv_obj_t *title = widgets::label(h, "Up next", l.font_title, color::text());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *back = widgets::round_button(h, LV_SYMBOL_RIGHT, l.btn_small, l.font_body, on_back);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);

    content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_coord_t top = l.margin / 2 + l.btn + l.gap;
    lv_obj_set_pos(content, l.margin, top);
    lv_obj_set_size(content, l.width - 2 * l.margin, l.height - top);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(content, l.margin, 0);
}

lv_obj_t *screen() { return scr; }

bool available() { return (caps & kCapQueue) != 0; }

void open() {
    if (device_id < 0) return;
    show_message("Loading...");
    commands::post(commands::Type::LoadQueue, device_id);
}

void update(const Device &d, uint8_t what) {
    if (d.id != device_id) {
        device_id = d.id;
        what      = kUpdateAll;
    }
    if (what & kUpdateDevice) caps = d.caps;
    // The queue shifts with every track; keep the old list until the new one arrives
    if ((what & kUpdateTrack) && lv_scr_act() == scr) commands::post(commands::Type::LoadQueue, device_id);
}

void set_items(int id, const QueueItem *items, int count) {
    if (id != device_id) return;
    if (count == 0) return show_message("Nothing queued");
    clear_rows();
    for (int i = 0; i < count && i < kMaxRows; i++) add_row(items[i], i);
    for (int i = 0; i < cover_count; i++) covers[i].wanted = artwork::thumb_request(token_of(i), covers[i].url);
}

void load_failed(int id, bool unsupported) {
    if (id != device_id) return;
    show_message(unsupported ? "No queue for this source" : "Couldn't load the queue");
}

}  // namespace ui::queue_screen
