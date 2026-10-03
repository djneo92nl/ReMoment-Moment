#include "queue_screen.h"

#include <Arduino.h>

#include "nav.h"
#include "net/commands.h"
#include "widgets.h"

namespace ui::queue_screen {

static Layout layout;
static lv_obj_t *scr;
static lv_obj_t *content;
static int       device_id = -1;
static uint16_t  caps      = 0;

static void on_back(lv_event_t *) { nav::to_now_playing(); }

static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_RIGHT) nav::to_now_playing();
}

static void show_message(const char *text) {
    lv_obj_clean(content);
    widgets::label(content, text, layout.font_body, color::muted());
}

// Title, then "artist · album" and the duration on the right
static void add_row(const QueueItem &it) {
    lv_obj_t *r = widgets::box(content);
    lv_obj_set_size(r, lv_pct(100), layout.row);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);

    char time[12] = "";
    if (it.duration > 0) snprintf(time, sizeof(time), "%d:%02d", it.duration / 60, it.duration % 60);
    lv_obj_t *d = widgets::label(r, time, layout.font_small, color::muted());
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_coord_t text_w = layout.width - 2 * layout.margin - layout.font_small->line_height * 3 - layout.gap;

    lv_obj_t *t = widgets::label(r, it.title, layout.font_body, color::text());
    // DOT only clips to one line when the height is fixed; otherwise the label wraps
    lv_obj_set_size(t, text_w, layout.font_body->line_height);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, -layout.gap / 2 - 4);

    char sub[112];
    if (it.artist[0] && it.album[0]) snprintf(sub, sizeof(sub), "%s - %s", it.artist, it.album);
    else snprintf(sub, sizeof(sub), "%s", it.artist[0] ? it.artist : it.album);
    lv_obj_t *s = widgets::label(r, sub, layout.font_small, color::muted());
    lv_obj_set_size(s, text_w, layout.font_small->line_height);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, layout.gap / 2 + 4);
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
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    if (count == 0) return show_message("Nothing queued");
    for (int i = 0; i < count; i++) add_row(items[i]);
}

void load_failed(int id, bool unsupported) {
    if (id != device_id) return;
    show_message(unsupported ? "No queue for this source" : "Couldn't load the queue");
}

}  // namespace ui::queue_screen
