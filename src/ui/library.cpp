#include "library.h"

#include <Arduino.h>

#include "artwork/artwork.h"
#include "nav.h"
#include "net/commands.h"
#include "widgets.h"

namespace ui::library {

static const int kThumbSize = 120;  // server's proxy_120
static const int kMaxThumbs = 240;  // tiles per view with artwork; only those near the screen hold pixels
static const int kMaxDepth  = 8;

enum class Mode : uint8_t { Home, Artist, Album, Playlist };
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
// Tab order on screen (the Moment puts Radio next to the playlists/collection tabs)
static const LibView kTabViews[] = {LibView::Recent, LibView::Playlists, LibView::Radio, LibView::Favorites,
                                    LibView::Artists};
static const char *const kTabNames[] = {"Recent", "Playlists", "Radio", "Favorites", "Artists"};
static const int kTabCount = 5;
static lv_obj_t *tab_btns[kTabCount];
static lv_obj_t *content;  // rebuilt per view
static lv_obj_t *grid;     // shelf or list that the next page of the current view goes into
static char      letter;   // Artists: letter of the A-Z bar jumped to, 0 = from the start

static Entry   stack[kMaxDepth];
static int     depth = 0;
static LibView tab   = LibView::Recent;

// Thumbnails: token = generation << 16 | seq << 12 | index into thumb_slots. Only tiles
// within a screen of the visible area hold a decoded image (a 160 px tile is 50 KB of
// PSRAM, a long shelf would run out); the others are dropped and asked for again when
// scrolled back. `seq` tells a stale delivery from the current request of the same slot.
static uint16_t generation = 1;
struct ThumbSlot {
    lv_obj_t  *box;
    lv_obj_t  *img;
    lv_coord_t size;
    uint8_t    seq;
    bool       wanted;  // requested (or showing)
    bool       large, half;
    char       url[192];
};
static ThumbSlot *thumb_slots;  // PSRAM, kMaxThumbs
static int thumb_count = 0;

static bool more_available = false;
static bool loading_more   = false;
static bool favorite       = false;

static const Entry &current() { return stack[depth - 1]; }

// ---- Building blocks ----------------------------------------------------------------

// A dark square that gets its image when the artwork task delivers it
static lv_obj_t *thumb(lv_obj_t *parent, lv_coord_t size, const char *url, bool large = false, bool half = false) {
    lv_obj_t *box = widgets::box(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    if (url && *url && thumb_slots && thumb_count < kMaxThumbs) {
        lv_obj_t *img = lv_img_create(box);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_pivot(img, 0, 0);
        ThumbSlot &slot = thumb_slots[thumb_count++];
        slot = {box, img, size, 0, false, large, half, ""};
        strlcpy(slot.url, url, sizeof(slot.url));
    }
    return box;
}

static uint32_t token_of(int index) {
    return (uint32_t)generation << 16 | (uint32_t)(thumb_slots[index].seq & 0xF) << 12 | index;
}

// Asks for the images of tiles near the screen and drops those far from it. Called after
// building a view and whenever a scroll ends.
static void update_thumbs() {
    if (thumb_count == 0) return;
    lv_obj_update_layout(scr);
    for (int i = 0; i < thumb_count; i++) {
        ThumbSlot &slot = thumb_slots[i];
        lv_area_t a;
        lv_obj_get_coords(slot.box, &a);
        bool near = a.x2 > -layout.width && a.x1 < 2 * layout.width && a.y2 > -layout.height &&
                    a.y1 < 2 * layout.height;
        if (near && !slot.wanted) {
            slot.seq    = (slot.seq + 1) & 0xF;
            slot.wanted = artwork::thumb_request(token_of(i), slot.url, slot.large, slot.half);
        } else if (!near && slot.wanted) {
            const void *src = lv_img_get_src(slot.img);
            lv_img_set_src(slot.img, nullptr);
            if (src) lv_img_cache_invalidate_src(src);
            artwork::thumb_drop(token_of(i));
            slot.wanted = false;
        }
    }
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
    tab    = (LibView)(intptr_t)lv_event_get_user_data(e);
    letter = 0;
    render();
}

static void on_album(lv_event_t *e) { push(Mode::Album, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_artist(lv_event_t *e) { push(Mode::Artist, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_playlist(lv_event_t *e) { push(Mode::Playlist, (int)(intptr_t)lv_event_get_user_data(e)); }

static void on_favorite(lv_event_t *) {
    favorite = !favorite;
    show_favorite();
    bool album = current().mode == Mode::Album;
    commands::post(album ? commands::Type::FavoriteAlbum : commands::Type::FavoriteArtist, -1, current().id,
                   nullptr, favorite);
}

// Device -1 = the current device (resolved on the loop task)
static commands::Type play_type() {
    return current().mode == Mode::Playlist ? commands::Type::PlayPlaylist : commands::Type::PlayAlbum;
}

static void on_play_album(lv_event_t *) {
    commands::post(play_type(), -1, current().id);
    nav::to_now_playing();
}

// Per-track play button
static void on_play_track(lv_event_t *e) {
    commands::post(play_type(), -1, current().id, nullptr, (int)(intptr_t)lv_event_get_user_data(e));
    nav::to_now_playing();
}

static void on_shuffle_artist(lv_event_t *) {
    commands::post(commands::Type::PlayArtist, -1, current().id);
    nav::to_now_playing();
}

static bool use_shelves() { return layout.landscape; }  // wide screens: shelves, portrait: lists

static void load_more() {
    if (!more_available || loading_more) return;
    loading_more = true;
    commands::post(commands::Type::LoadLibrary, -1, (int)tab, "more");
}

// Lists page downwards
static void on_scroll_end(lv_event_t *) {
    update_thumbs();
    if (!use_shelves() && lv_obj_get_scroll_bottom(content) <= layout.row * 2) load_more();
}

// Shelves page sideways (only the one marked as paged: Artists, Playlists)
static void on_shelf_scroll_end(lv_event_t *e) {
    update_thumbs();
    lv_obj_t *row = lv_event_get_target(e);
    if (lv_event_get_user_data(e) && lv_obj_get_scroll_right(row) <= layout.tile * 3) load_more();
}

static void letter_command(char *buf, size_t size) {
    snprintf(buf, size, "letter:%c", letter);
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
    lv_obj_set_style_pad_top(content, 0, 0);  // the Radio row adds its own
    lv_obj_set_size(content, layout.width - 2 * layout.margin, layout.height - top);

    if (home) {
        lv_label_set_text(title_label, "Library");
        for (int i = 0; i < kTabCount; i++) {
            bool on = kTabViews[i] == tab;
            lv_obj_set_style_text_color(tab_btns[i], on ? color::text() : color::muted(), 0);
        }
        char text[12] = "";
        if (tab == LibView::Artists && letter) letter_command(text, sizeof(text));  // back from an artist
        commands::post(commands::Type::LoadLibrary, -1, (int)tab, text);
    } else {
        lv_label_set_text(title_label, "");
        commands::Type load = e.mode == Mode::Artist     ? commands::Type::LoadArtist
                            : e.mode == Mode::Playlist ? commands::Type::LoadPlaylist
                                                       : commands::Type::LoadAlbum;
        commands::post(load, -1, e.id);
    }
    show_message("Loading...");
}

// ---- Content --------------------------------------------------------------------

// Device -1 = the current device (resolved on the loop task)
static void on_station(lv_event_t *e) {
    commands::post(commands::Type::PlayRadio, -1, (int)(intptr_t)lv_event_get_user_data(e));
    nav::to_now_playing();
}

static lv_event_cb_t open_cb(LibKind kind) {
    return kind == LibKind::Artist     ? on_artist
         : kind == LibKind::Playlist ? on_playlist
         : kind == LibKind::Station  ? on_station
                                     : on_album;
}

// A station without artwork: a light square with a note, like the Moment's placeholder
static void station_placeholder(lv_obj_t *art, const LibItem &it) {
    if (it.kind != LibKind::Station || it.cover[0] || it.thumb[0]) return;
    lv_obj_set_style_bg_color(art, lv_color_hex(0xA9B6C9), 0);
    lv_obj_center(widgets::label(art, LV_SYMBOL_AUDIO, layout.font_title, lv_color_hex(0x7A889C)));
}

// Shelf tile: art with the name centred below, Moment-style. Tiles above 120 px use the
// 320 cover decoded at half size, so nothing is scaled at draw time.
static void add_tile(lv_obj_t *parent, const LibItem &it) {
    lv_coord_t size = layout.tile;
    lv_obj_t *tile = widgets::box(parent);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, size, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 4, 0);
    lv_obj_set_style_opa(tile, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(tile, open_cb(it.kind), LV_EVENT_CLICKED, (void *)(intptr_t)it.id);

    lv_obj_t *art = size > kThumbSize && it.cover[0] ? thumb(tile, size, it.cover, true, true) : thumb(tile, size, it.thumb);
    station_placeholder(art, it);
    lv_obj_t *t = widgets::label(tile, it.title, layout.font_small, color::text());
    lv_obj_set_width(t, size);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *s = widgets::label(tile, it.subtitle, layout.font_small, color::muted());
    lv_obj_set_width(s, size);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
}

// List row: small art, name and subtitle
static void add_row(lv_obj_t *parent, const LibItem &it) {
    lv_obj_t *r = widgets::box(parent);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, lv_pct(100), layout.row);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(r, color::text(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, open_cb(it.kind), LV_EVENT_CLICKED, (void *)(intptr_t)it.id);

    lv_coord_t size = layout.row - layout.gap;
    lv_obj_t *th = thumb(r, size, it.thumb);
    station_placeholder(th, it);
    lv_obj_align(th, LV_ALIGN_LEFT_MID, 0, 0);
    lv_coord_t text_w = layout.width - 2 * layout.margin - size - layout.gap;
    lv_obj_t *t = widgets::label(r, it.title, layout.font_body, color::text());
    lv_obj_set_width(t, text_w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, size + layout.gap, -layout.gap / 2 - 4);
    lv_obj_t *s = widgets::label(r, it.subtitle, layout.font_small, color::muted());
    lv_obj_set_width(s, text_w);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, size + layout.gap, layout.gap / 2 + 4);
}

static void add_item(const LibItem &it) { use_shelves() ? add_tile(grid, it) : add_row(grid, it); }

// A titled section with the items of one shelf: a sideways-scrolling row of tiles on wide
// screens, a list of rows on portrait ones. `paged`: loads the next page at its end.
// Returns the number of items shown.
static int add_section(const char *title, const LibItem *items, int count, uint8_t shelf, bool paged = false) {
    int n = 0;
    for (int i = 0; i < count; i++) n += items[i].shelf == shelf;
    if (n == 0) return 0;
    section_title(title);
    grid = lv_obj_create(content);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    if (use_shelves()) {
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(grid, layout.gap, 0);
        lv_obj_set_scroll_dir(grid, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(grid, on_shelf_scroll_end, LV_EVENT_SCROLL_END, (void *)(intptr_t)paged);
    } else {
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    }
    for (int i = 0; i < count; i++)
        if (items[i].shelf == shelf) add_item(items[i]);
    return n;
}

// ---- Artists: A-Z bar under the shelf ------------------------------------------------

static const char kLetters[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void on_letter(lv_event_t *e) {
    letter = (char)(intptr_t)lv_event_get_user_data(e);
    char text[12];
    letter_command(text, sizeof(text));
    commands::post(commands::Type::LoadLibrary, -1, (int)LibView::Artists, text);
}

static void add_letter_bar() {
    lv_obj_t *bar = widgets::box(content);
    lv_obj_set_size(bar, lv_pct(100), layout.btn_small);
    lv_obj_set_style_pad_top(bar, layout.gap, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char text[2] = "";
    for (const char *c = kLetters; *c; c++) {
        text[0] = *c;
        bool on = *c == letter || (!letter && *c == '#');
        lv_obj_t *l = widgets::label(bar, text, layout.font_body, on ? color::text() : color::muted());
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(l, layout.gap);
        lv_obj_set_style_opa(l, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_event_cb(l, on_letter, LV_EVENT_CLICKED, (void *)(intptr_t)*c);
    }
}

void set_items(LibView view, const LibItem *items, int count, bool append, bool more) {
    if (current().mode != Mode::Home || view != tab) return;
    more_available = more;
    loading_more   = false;

    if (append) {
        if (grid)
            for (int i = 0; i < count; i++) add_item(items[i]);
        return update_thumbs();
    }

    clear_content();
    more_available = more;
    switch (view) {
        case LibView::Recent:
            if (count == 0) show_message("Nothing played yet");
            add_section("RECENTLY PLAYED", items, count, 0);
            add_section("FAVORITE ALBUMS", items, count, 1);
            add_section("FAVORITE ARTISTS", items, count, 2);
            break;
        case LibView::Favorites:
            if (count == 0) show_message("No favorites yet");
            add_section("ALBUMS", items, count, 1);
            add_section("ARTISTS", items, count, 2);
            break;
        case LibView::Radio:
            if (!add_section("STATIONS", items, count, 0)) show_message("No radio stations for this device");
            break;
        case LibView::Playlists:
            if (!add_section("PLAYLISTS", items, count, 0, true)) show_message("No playlists");
            break;
        case LibView::Artists:
            if (!add_section("ARTISTS", items, count, 0, true)) show_message("No artists");
            if (use_shelves()) add_letter_bar();  // a portrait list is scrolled instead
            break;
    }
    update_thumbs();
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

    if (!add_section("ALBUMS", albums, count, 0)) show_message("No albums");
    update_thumbs();
}

static void format_duration(char *buf, size_t size, int seconds) {
    if (seconds < 0) buf[0] = '\0';
    else snprintf(buf, size, "%d:%02d", seconds / 60, seconds % 60);
}

void set_album(const LibAlbum &album, const LibTrack *tracks, int count) {
    Mode expected = album.is_playlist ? Mode::Playlist : Mode::Album;
    if (current().mode != expected || current().id != album.id) return;
    clear_content();
    lv_label_set_text(title_label, album.name);
    favorite = album.favorite;
    show_favorite();
    set_visible(favorite_btn, !album.is_playlist);  // favorites are albums and artists only

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
    if (album.year > 0) snprintf(sub, sizeof(sub), "%s - %d", album.artist, album.year);
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
        bool playable = t.playable && album.playable;
        if (!playable) lv_obj_set_style_opa(r, LV_OPA_40, 0);
        char num[8], dur[12];
        snprintf(num, sizeof(num), "%d", i + 1);
        format_duration(dur, sizeof(dur), t.duration);
        if (playable) {
            lv_obj_t *play = widgets::label(r, LV_SYMBOL_PLAY, layout.font_small, color::text());
            lv_obj_add_flag(play, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(play, layout.gap);
            lv_obj_set_style_opa(play, LV_OPA_50, LV_STATE_PRESSED);
            lv_obj_add_event_cb(play, on_play_track, LV_EVENT_CLICKED, (void *)(intptr_t)t.id);
            lv_obj_align(play, LV_ALIGN_LEFT_MID, 0, 0);
        }
        lv_obj_t *n = widgets::label(r, num, layout.font_small, color::muted());
        lv_obj_align(n, LV_ALIGN_LEFT_MID, layout.margin * 3 / 4, 0);
        lv_obj_t *name = widgets::label(r, t.name, layout.font_body, color::text());
        lv_obj_set_width(name, lv_pct(album.is_playlist ? 45 : 65));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, layout.margin * 3 / 2, 0);
        if (album.is_playlist) {  // mixed artists: show each track's
            lv_obj_t *a = widgets::label(r, t.artist, layout.font_small, color::muted());
            lv_obj_set_width(a, lv_pct(30));
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_set_align(a, LV_ALIGN_LEFT_MID);
            lv_obj_set_x(a, lv_pct(58));
        }
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
    int index = token & 0xFFF;
    if ((token >> 16) != generation || index >= thumb_count) return false;
    ThumbSlot &slot = thumb_slots[index];
    if (!slot.wanted || slot.seq != ((token >> 12) & 0xF)) return false;  // dropped meanwhile
    lv_img_set_src(slot.img, image);
    if (image->header.w > 0) lv_img_set_zoom(slot.img, (uint32_t)slot.size * LV_IMG_ZOOM_NONE / image->header.w);
    return true;
}

// ---------------------------------------------------------------------------------

void build(const Layout &l) {
    layout = l;
    thumb_slots = (ThumbSlot *)ps_calloc(kMaxThumbs, sizeof(ThumbSlot));
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

    // Tabs like the Moment's: plain words centred at the top, the selected one white
    tabs_row = widgets::box(scr);
    lv_obj_set_size(tabs_row, LV_SIZE_CONTENT, l.btn_small);
    lv_obj_align(tabs_row, LV_ALIGN_TOP_MID, 0, l.margin / 2 + l.btn);
    lv_obj_set_flex_flow(tabs_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs_row, l.landscape ? l.margin : l.gap, 0);
    for (int i = 0; i < kTabCount; i++) {
        // Five tabs don't fit the 3.5" in the body font
        lv_obj_t *t = widgets::label(tabs_row, kTabNames[i], l.landscape ? l.font_body : l.font_small, color::muted());
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(t, l.gap);
        lv_obj_add_event_cb(t, on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)kTabViews[i]);
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
