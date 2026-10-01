// Library screen (swipe down from Now Playing): Recent / Favorites / Artists tabs,
// artist and album views. One screen whose content is rebuilt per view, with a small
// back stack. Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "ui.h"

namespace ui::library {

void build(const Layout &layout);
lv_obj_t *screen();

// Shows the home tabs (keeps the last selected tab)
void open_home();

void set_items(LibView view, const LibItem *items, int count, bool append, bool more);
void set_artist(int artist_id, const char *name, bool favorite, const LibItem *albums, int count);
void set_album(const LibAlbum &album, const LibTrack *tracks, int count);
void load_failed();
bool thumb_ready(uint32_t token, const lv_img_dsc_t *image);

}  // namespace ui::library
