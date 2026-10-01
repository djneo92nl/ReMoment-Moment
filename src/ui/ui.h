// UI layer. Every function here takes the LVGL lock itself, so call them from any task
// except the LVGL task (event callbacks there already hold the lock). User input goes
// out through net/commands, never by touching the model from the LVGL task.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "model.h"

namespace ui {

void init();

// Startup / setup / pairing screen: logo, a status line and an optional big code.
void show_status(const char *message, const char *detail = nullptr, const char *code = nullptr);

void show_now_playing();

// What changed on a device, for now_playing_update()
enum : uint8_t {
    kUpdateTrack    = 1 << 0,
    kUpdateProgress = 1 << 1,
    kUpdateState    = 1 << 2,
    kUpdateVolume   = 1 << 3,
    kUpdateModes    = 1 << 4,
    kUpdateDevice   = 1 << 5,  // name, capabilities
    kUpdateAll      = 0xFF,
};

// Renders the parts of `device` named by `what` on Now Playing (the current device only).
void now_playing_update(const Device &device, uint8_t what);
void now_playing_set_notice(const char *text);  // small hint line, nullptr = hide

// Device list (swipe up): the full list, and single-row refreshes.
void devices_sync(const Device *devices, uint8_t count, int current);
void devices_update(int index, const Device &device);

// Details screen of one device: options loaded by commands::Type::LoadDetails.
enum class Section : uint8_t { Sources, Radio, Group };
struct Option {
    char id[48];
    char label[48];
    char detail[48];
    bool active;
};
void details_set_options(int device_id, Section section, const Option *options, int count);

// Library (swipe down from Now Playing). Data is loaded by commands::Type::Load*.
enum class LibView : uint8_t { Recent, Favorites, Artists, Radio, Playlists };
enum class LibKind : uint8_t { Album, Artist, Station, Playlist };
struct LibItem {
    LibKind kind;
    int     id;
    char    title[64];
    char    subtitle[64];
    char    thumb[192];  // proxy_120 URL, "" = none
    char    cover[192];  // proxy_320 URL, for tiles larger than 120 px
    bool    favorite;
    uint8_t shelf;       // Recent: 0 recently played, 1 favorite albums, 2 favorite artists
};
struct LibTrack {
    int  id;
    char name[64];
    char artist[48];  // playlists only (albums have one artist)
    int  duration;  // seconds, -1 = unknown
    bool playable;
};
struct LibAlbum {
    int  id;
    char name[64];
    int  artist_id;
    char artist[64];
    int  year;        // 0 = unknown
    char cover[192];  // proxy_320 URL
    bool favorite;
    bool playable;
    bool is_playlist;  // artist = owner, tracks carry their own artist, no favorites
};
// `append` adds a page to the list; `more` = another page can be loaded
void library_set_items(LibView view, const LibItem *items, int count, bool append, bool more);
void library_set_artist(int artist_id, const char *name, bool favorite, const LibItem *albums, int count);
void library_set_album(const LibAlbum &album, const LibTrack *tracks, int count);
void library_load_failed();
// From the artwork task: a thumbnail for `token` is decoded. Returns false when the
// screen no longer wants it (the caller then frees the image).
bool library_thumb(uint32_t token, const lv_img_dsc_t *image);

// Settings screen (swipe up from the device list): values come from settings::value(),
// the info section from the loop task (commands::Type::LoadSettingsInfo).
struct SettingsInfo {
    char board[32];
    char firmware[24];
    char wifi[33];
    char ip[16];
    char settings_url[32];
    char password[48];
    char server[96];
    char mqtt[24];
};
void settings_set_info(const SettingsInfo &info);

// Nothing-playing screen: call about once a second from the loop task. Replaces Now Playing
// while the current device isn't playing (after a short grace), and gives way to Now Playing
// when playback starts.
void idle_check(const Device *device);

// Short message in the middle of the screen (errors, confirmations)
void toast(const char *text);

// Artwork (called from the artwork task). Images stay owned by the caller:
// artwork_show() displays them (cross-fade for changed ones, nullptr = placeholder),
// artwork_release() guarantees an image is no longer referenced before it is overwritten.
void artwork_show(const lv_img_dsc_t *cover, bool cover_changed, const lv_img_dsc_t *background,
                  bool background_changed, bool has_accent, uint32_t accent);
void artwork_release(const lv_img_dsc_t *image);

// Small icon in the top-left corner while something is down; hidden when all is well.
void set_connection(bool wifi, bool server, bool mqtt);

}  // namespace ui
