#include "library.h"

#include <Arduino.h>

#include "artwork/artwork.h"
#include "nav.h"
#include "net/commands.h"
#include "widgets.h"

namespace ui::library {

static const int kThumbSize = 120;  // server's proxy_120
static const int kMaxThumbs = 96;   // matches the artwork task's limit
static const int kMaxDepth  = 8;

enum class Mode : uint8_t { Home, Artist, Album };
struct Entry {
    Mode mode;
    int  id;
};

static Layout layout;
static lv_obj_t *scr;
static lv_obj_t *back_btn;
static lv_obj_t *title_label;
static lv_obj_t *favorite_btn;
static lv_obj_t *tabs_row;
static lv_obj_t *tab_btns[3];
static lv_obj_t *content;  // rebuilt per view
static lv_obj_t *grid;     // album tiles / artist rows of the current view

static Entry   stack[kMaxDepth];
static int     depth = 0;
static LibView tab   = LibView::Recent;

// Thumbnails: token = generation << 16 | index into thumb_slots
static uint16_t generation = 1;
struct ThumbSlot {
    lv_obj_t  *img;
    lv_coord_t size;
};
static ThumbSlot thumb_slots[kMaxThumbs];
static int thumb_count = 0;

static bool more_available = false;
static bool loading_more   = false;
static bool favorite       = false;

static const Entry &current() { return stack[depth - 1]; }

// ---- Building blocks ----------------------------------------------------------------

// A dark square that gets its image when the artwork task delivers it
static lv_obj_t *thumb(lv_obj_t *parent, lv_coord_t size, const char *url, bool large = false) {
    lv_obj_t *box = widgets::box(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    if (url && *url && thumb_count < kMaxThumbs) {
        lv_obj_t *img = lv_img_create(box);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(img, 0, 0);
        int index = thumb_count++;
        thumb_slots[index] = {img, size};
        artwork::thumb_request((uint32_t)generation << 16 | index, url, large);
    }
    return box;
}

static void show_message(const char *text) {
    widgets::label(content, text, layout.font_body, color::muted());
}

static void set_visible(lv_obj_t *obj, bool visible) {
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show_favorite() {
    widgets::set_symbol(favorite_btn, favorite ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
    widgets::set_active(favorite_btn, favorite);
}

// Drops the old view: objects first, then tell the artwork task their images can go
static void clear_content() {
    lv_obj_clean(content);
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    grid        = nullptr;
    thumb_count = 0;
    generation  = generation == 0xFFFF ? 1 : generation + 1;
    lv_img_cache_invalidate_src(nullptr);
    artwork::thumbs_release(generation);
    more_available = loading_more = false;
}

static lv_obj_t *new_grid() {
    grid = widgets::box(content);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, layout.gap, 0);
    lv_obj_set_style_pad_row(grid, layout.gap, 0);
    return grid;
}

static lv_obj_t *new_list() {
    grid = widgets::box(content);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    return grid;
}

static void section_title(const char *text) {
    lv_obj_t *t = widgets::label(content, text, layout.font_small, color::muted());
    lv_obj_set_style_pad_ver(t, layout.gap, 0);
}

// ---- Navigation inside the library -------------------------------------------------

static void render();

static void push(Mode mode, int id) {
    if (depth < kMaxDepth) stack[depth++] = {mode, id};
    else stack[depth - 1] = {mode, id};
    render();
}

static void pop() {
    if (depth > 1) {
        depth--;
        render();
    } else {
        nav::to_now_playing();
    }
}

static void on_back(lv_event_t *) { pop(); }
static void on_close(lv_event_t *) { nav::to_now_playing(); }

static void on_gesture(lv_dir_t dir) {
    if (dir == LV_DIR_TOP) nav::to_now_playing();
    else if (dir == LV_DIR_RIGHT) pop();
}

static void on_tab(lv_event_t *e) {
    tab = (LibView)(intptr_t)lv_event_get_user_data(e);
    render();
}

static void on_album(lv_event_t *e) { push(Mode::Album, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_artist(lv_event_t *e) { push(Mode::Artist, (int)(intptr_t)lv_event_get_user_data(e)); }

static void on_favorite(lv_event_t *) {
    favorite = !favorite;
    show_favorite();
    bool album = current().mode == Mode::Album;
    commands::post(album ? commands::Type::FavoriteAlbum : commands::Type::FavoriteArtist, -1, current().id,
                   nullptr, favorite);
}

// Device -1 = the current device (resolved on the loop task)
static void on_play_album(lv_event_t *) {
    commands::post(commands::Type::PlayAlbum, -1, current().id);
    nav::to_now_playing();
}

static void on_play_track(lv_event_t *e) {
    commands::post(commands::Type::PlayAlbum, -1, current().id, nullptr, (int)(intptr_t)lv_event_get_user_data(e));
    nav::to_now_playing();
}

static void on_shuffle_artist(lv_event_t *) {
    commands::post(commands::Type::PlayArtist, -1, current().id);
    nav::to_now_playing();
}

static void on_scroll_end(lv_event_t *) {
    if (!more_available || loading_more) return;
    if (lv_obj_get_scroll_bottom(content) > layout.row * 2) return;
    loading_more = true;
    commands::post(commands::Type::LoadLibrary, -1, (int)LibView::Artists, "more");
}

static void render() {
    clear_content();
    const Entry &e = current();
    bool home = e.mode == Mode::Home;
    set_visible(back_btn, !home);
    set_visible(tabs_row, home);
    set_visible(favorite_btn, !home);

    lv_coord_t top = layout.margin / 2 + layout.btn + (home ? layout.btn_small + layout.gap : layout.gap);
    lv_obj_set_pos(content, layout.margin, top);
    lv_obj_set_size(content, layout.width - 2 * layout.margin, layout.height - top);

    if (home) {
        lv_label_set_text(title_label, "Library");
        for (int i = 0; i < 3; i++) {
            bool on = (int)tab == i;
            lv_obj_set_style_text_color(tab_btns[i], on ? color::text() : color::muted(), 0);
            lv_obj_set_style_border_opa(tab_btns[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        commands::post(commands::Type::LoadLibrary, -1, (int)tab);
    } else {
        lv_label_set_text(title_label, "");
        commands::post(e.mode == Mode::Artist ? commands::Type::LoadArtist : commands::Type::LoadAlbum, -1, e.id);
    }
    show_message("Loading\xE2\x80\xA6");
}

// ---- Content --------------------------------------------------------------------

static void add_album_tile(const LibItem &it) {
    lv_obj_t *tile = widgets::box(grid);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, kThumbSize, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tile, 4, 0);
    lv_obj_set_style_opa(tile, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(tile, on_album, LV_EVENT_CLICKED, (void *)(intptr_t)it.id);

    thumb(tile, kThumbSize, it.thumb);
    lv_obj_t *t = widgets::label(tile, it.title, layout.font_small, color::text());
    lv_obj_set_width(t, kThumbSize);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_t *s = widgets::label(tile, it.subtitle, layout.font_small, color::muted());
    lv_obj_set_width(s, kThumbSize);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
}

static void add_artist_row(const LibItem &it) {
    lv_obj_t *r = widgets::box(grid);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, lv_pct(100), layout.row);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(r, color::text(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, on_artist, LV_EVENT_CLICKED, (void *)(intptr_t)it.id);

    lv_coord_t size = layout.row - layout.gap;
    lv_obj_t *th = thumb(r, size, it.thumb);
    lv_obj_align(th, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *t = widgets::label(r, it.title, layout.font_body, color::text());
    lv_obj_align(t, LV_ALIGN_LEFT_MID, size + layout.gap, -layout.gap / 2 - 4);
    lv_obj_t *s = widgets::label(r, it.subtitle, layout.font_small, color::muted());
    lv_obj_align(s, LV_ALIGN_LEFT_MID, size + layout.gap, layout.gap / 2 + 4);
}

void set_items(LibView view, const LibItem *items, int count, bool append, bool more) {
    if (current().mode != Mode::Home || view != tab) return;
    if (!append) clear_content();
    more_available = more;
    loading_more   = false;

    if (view == LibView::Favorites) {
        int albums = 0;
        for (int i = 0; i < count; i++) albums += items[i].kind == LibKind::Album;
        if (count == 0) show_message("No favorites yet");
        if (albums > 0) {
            section_title("ALBUMS");
            new_grid();
            for (int i = 0; i < count; i++)
                if (items[i].kind == LibKind::Album) add_album_tile(items[i]);
        }
        if (count > albums) {
            section_title("ARTISTS");
            new_list();
            for (int i = 0; i < count; i++)
                if (items[i].kind == LibKind::Artist) add_artist_row(items[i]);
        }
        return;
    }

    if (!grid) {
        if (count == 0) return show_message(view == LibView::Recent ? "Nothing played yet" : "No artists");
        view == LibView::Recent ? new_grid() : new_list();
    }
    for (int i = 0; i < count; i++) {
        if (items[i].kind == LibKind::Album) add_album_tile(items[i]);
        else add_artist_row(items[i]);
    }
}

void set_artist(int artist_id, const char *name, bool fav, const LibItem *albums, int count) {
    if (current().mode != Mode::Artist || current().id != artist_id) return;
    clear_content();
    lv_label_set_text(title_label, name);
    favorite = fav;
    show_favorite();

    lv_obj_t *actions = widgets::box(content);
    lv_obj_set_size(actions, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(actions, layout.gap, 0);
    lv_obj_t *shuffle = widgets::round_button(actions, LV_SYMBOL_SHUFFLE, layout.btn, layout.font_body, on_shuffle_artist);
    lv_obj_align(shuffle, LV_ALIGN_LEFT_MID, 0, 0);

    section_title("ALBUMS");
    new_grid();
    for (int i = 0; i < count; i++) add_album_tile(albums[i]);
    if (count == 0) show_message("No albums");
}

static void format_duration(char *buf, size_t size, int seconds) {
    if (seconds < 0) buf[0] = '\0';
    else snprintf(buf, size, "%d:%02d", seconds / 60, seconds % 60);
}

void set_album(const LibAlbum &album, const LibTrack *tracks, int count) {
    if (current().mode != Mode::Album || current().id != album.id) return;
    clear_content();
    lv_label_set_text(title_label, album.name);
    favorite = album.favorite;
    show_favorite();

    // Landscape: cover and actions left, tracks right. Portrait: stacked.
    lv_obj_t *body = widgets::box(content);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, layout.landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_column(body, layout.margin, 0);
    lv_obj_set_style_pad_row(body, layout.gap, 0);

    lv_obj_t *side = widgets::box(body);
    lv_obj_set_size(side, layout.cover, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, layout.gap, 0);
    thumb(side, layout.cover, album.cover, true);
    char sub[96];
    if (album.year > 0) snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %d", album.artist, album.year);
    else strlcpy(sub, album.artist, sizeof(sub));
    lv_obj_t *artist = widgets::label(side, sub, layout.font_body, color::muted());
    lv_obj_set_width(artist, layout.cover);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    if (album.playable) {
        widgets::round_button(side, LV_SYMBOL_PLAY, layout.btn, layout.font_body, on_play_album);
    } else {
        widgets::label(side, "Not playable on this device", layout.font_small, color::muted());
    }

    lv_obj_t *list = widgets::box(body);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < count; i++) {
        const LibTrack &t = tracks[i];
        lv_obj_t *r = widgets::box(list);
        lv_obj_set_size(r, lv_pct(100), layout.row * 2 / 3);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
        if (t.playable && album.playable) {
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(r, color::text(), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
            lv_obj_add_event_cb(r, on_play_track, LV_EVENT_CLICKED, (void *)(intptr_t)t.id);
        } else {
            lv_obj_set_style_opa(r, LV_OPA_40, 0);
        }
        char num[8], dur[12];
        snprintf(num, sizeof(num), "%d", i + 1);
        format_duration(dur, sizeof(dur), t.duration);
        lv_obj_t *n = widgets::label(r, num, layout.font_small, color::muted());
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *name = widgets::label(r, t.name, layout.font_body, color::text());
        lv_obj_set_width(name, lv_pct(75));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, layout.margin, 0);
        lv_obj_t *d = widgets::label(r, dur, layout.font_small, color::muted());
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void load_failed() {
    loading_more = false;
    if (lv_obj_get_child_cnt(content) <= 1) {
        lv_obj_clean(content);
        show_message("Couldn't load the library");
    } else {
        nav::toast("Couldn't load the library");
    }
}

bool thumb_ready(uint32_t token, const lv_img_dsc_t *image) {
    int index = token & 0xFFFF;
    if ((token >> 16) != generation || index >= thumb_count) return false;
    ThumbSlot &slot = thumb_slots[index];
    lv_img_set_src(slot.img, image);
    if (image->header.w > 0) lv_img_set_zoom(slot.img, (uint32_t)slot.size * LV_IMG_ZOOM_NONE / image->header.w);
    return true;
}

// ---------------------------------------------------------------------------------

void build(const Layout &l) {
    layout = l;
    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, color::bg(), 0);
    widgets::on_gesture(scr, on_gesture);

    lv_obj_t *h = widgets::box(scr);
    lv_obj_set_size(h, l.width - 2 * l.margin, l.btn);
    lv_obj_set_pos(h, l.margin, l.margin / 2);
    back_btn = widgets::round_button(h, LV_SYMBOL_LEFT, l.btn_small, l.font_body, on_back);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    title_label = widgets::label(h, "Library", l.font_title, color::text());
    lv_obj_set_width(title_label, l.width - 2 * l.margin - 3 * (l.btn_small + l.gap));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, l.btn_small + l.gap, 0);
    lv_obj_t *close = widgets::round_button(h, LV_SYMBOL_UP, l.btn_small, l.font_body, on_close);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    favorite_btn = widgets::round_button(h, LV_SYMBOL_PLUS, l.btn_small, l.font_body, on_favorite);
    lv_obj_align(favorite_btn, LV_ALIGN_RIGHT_MID, -(l.btn_small + l.gap), 0);

    // Tabs: plain words, the selected one white and underlined
    tabs_row = widgets::box(scr);
    lv_obj_set_size(tabs_row, LV_SIZE_CONTENT, l.btn_small);
    lv_obj_set_pos(tabs_row, l.margin, l.margin / 2 + l.btn);
    lv_obj_set_flex_flow(tabs_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs_row, l.margin, 0);
    static const char *kTabs[] = {"Recent", "Favorites", "Artists"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *t = widgets::label(tabs_row, kTabs[i], l.font_body, color::muted());
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(t, l.gap);
        lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_border_color(t, color::text(), 0);
        lv_obj_set_style_pad_bottom(t, 6, 0);
        lv_obj_add_event_cb(t, on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tab_btns[i] = t;
    }

    content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(content, l.margin, 0);
    lv_obj_add_event_cb(content, on_scroll_end, LV_EVENT_SCROLL_END, nullptr);

    stack[0] = {Mode::Home, 0};
    depth    = 1;
}

lv_obj_t *screen() { return scr; }

void open_home() {
    depth = 1;
    render();
}

}  // namespace ui::library
