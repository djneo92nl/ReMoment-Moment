#include "commands.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <WiFi.h>
#include <garnet_web.h>

#include "api.h"
#include "app_config.h"
#include "board/board.h"
#include "model.h"
#include "player.h"
#include "app_settings.h"
#include "ui/ui.h"

namespace commands {

static const unsigned long kVolumeIntervalMs = 200;  // max rate while dragging the slider

static QueueHandle_t queue;

// Volume is coalesced outside the queue: dragging produces many values, only the last counts
static portMUX_TYPE volume_mux = portMUX_INITIALIZER_UNLOCKED;
static int volume_device       = -1;
static int volume_value        = -1;
static unsigned long last_volume_ms = 0;

void begin() { queue = xQueueCreate(16, sizeof(Command)); }

void post(Type type, int device_id, int value, const char *text, int value2) {
    if (type == Type::Volume) {
        portENTER_CRITICAL(&volume_mux);
        volume_device = device_id;
        volume_value  = value;
        portEXIT_CRITICAL(&volume_mux);
        return;
    }
    Command c = {type, device_id, value, value2, ""};
    if (text) strlcpy(c.text, text, sizeof(c.text));
    if (queue && xQueueSend(queue, &c, 0) != pdTRUE) Serial.println("[commands] queue full, dropped");
}

static String device_path(int device_id, const char *suffix) {
    return String("/devices/") + device_id + "/" + suffix;
}

static const char *device_name(int device_id) {
    int i = model.index_of(device_id);
    return i >= 0 ? model.device_at(i)->name : "the device";
}

static remoment::Control &ctl() { return player::client().control(); }

// Turns an API failure into a short message for the toast
static bool report(int status, int device_id, const JsonDocument &res) {
    if (api::ok(status)) return true;
    char msg[96];
    remoment::describeError(status, res, device_name(device_id), msg, sizeof(msg));
    ui::toast(msg);
    return false;
}

// For Control calls, which only hand back the status
static bool report(int status, int device_id) {
    static const JsonDocument none;
    return report(status, device_id, none);
}

// ---- Volume and queue screens -------------------------------------------------------

// Quiet on failure: the source line just stays hidden
static void load_source(int device_id) {
    JsonDocument res;
    const char *label = "";
    if (api::ok(ctl().sources(device_id, res))) {
        for (JsonObjectConst s : res["sources"].as<JsonArrayConst>()) {
            if (s["in_use"] | false) {
                const char *name = s["friendly_name"] | "";
                label            = *name ? name : (s["source_id"] | "");
                break;
            }
        }
    }
    ui::volume_set_source(device_id, label);
}

static const int kMaxQueue = 20;

static void load_queue(int device_id) {
    static ui::QueueItem *items;
    if (!items) items = (ui::QueueItem *)ps_calloc(kMaxQueue, sizeof(ui::QueueItem));
    if (!items) return ui::queue_load_failed(device_id, false);

    JsonDocument res(api::psram());
    int status = ctl().queue(device_id, kMaxQueue, res);
    if (!api::ok(status)) return ui::queue_load_failed(device_id, status == 422);

    int n = 0;
    for (JsonObjectConst q : res["up_next"].as<JsonArrayConst>()) {
        if (n >= kMaxQueue) break;
        ui::QueueItem &it = items[n++];
        strlcpy(it.title, q["name"] | "", sizeof(it.title));
        strlcpy(it.artist, q["artist"] | "", sizeof(it.artist));
        strlcpy(it.album, q["album"] | "", sizeof(it.album));
        it.duration = q["duration"] | -1;
        strlcpy(it.thumb, q["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    }
    ui::queue_set_items(device_id, items, n);
}

// ---- Details screen data ---------------------------------------------------------

static const int kMaxOptions = 32;

static void load_details(int device_id) {
    int i = model.index_of(device_id);
    if (i < 0) return;
    const Device &d = *model.device_at(i);
    static ui::Option options[kMaxOptions];

    if (d.has(kCapSourceActivation)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(ctl().sources(device_id, res))) {
            for (JsonObjectConst s : res["sources"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                strlcpy(o.id, s["source_id"] | "", sizeof(o.id));
                strlcpy(o.label, s["friendly_name"] | o.id, sizeof(o.label));
                strlcpy(o.detail, s["provider_name"] | "", sizeof(o.detail));
                o.active = s["in_use"] | false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Sources, options, n);
    }

    if (d.has(kCapRadio)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(ctl().radioStations(device_id, res))) {
            for (JsonObjectConst s : res["stations"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                snprintf(o.id, sizeof(o.id), "%d", s["id"] | 0);
                strlcpy(o.label, s["name"] | "", sizeof(o.label));
                strlcpy(o.detail, s["genre"] | "", sizeof(o.detail));
                o.active = false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Radio, options, n);
    }

    if (d.has(kCapMultiroom)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(api::get(device_path(device_id, "multiroom").c_str(), res))) {
            for (JsonObjectConst s : res["joinable"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                snprintf(o.id, sizeof(o.id), "%d", s["id"] | 0);
                strlcpy(o.label, s["device_name"] | "", sizeof(o.label));
                strlcpy(o.detail, s["state"] | "", sizeof(o.detail));
                o.active = false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Group, options, n);
    }
}

// ---- Library ----------------------------------------------------------------------

static const int kMaxItems  = 60;
static const int kMaxTracks = 100;

static ui::LibItem  *items;   // PSRAM, allocated on first use
static ui::LibTrack *tracks;
static String        artists_cursor;    // next page of the artist list, "" = none
static String        playlists_cursor;  // next page of the playlist list

static bool ensure_buffers() {
    if (!items) items = (ui::LibItem *)ps_calloc(kMaxItems, sizeof(ui::LibItem));
    if (!tracks) tracks = (ui::LibTrack *)ps_calloc(kMaxTracks, sizeof(ui::LibTrack));
    return items && tracks;
}

static void album_item(JsonObjectConst a, ui::LibItem &it) {
    it.kind = ui::LibKind::Album;
    it.id   = a["id"] | 0;
    strlcpy(it.title, a["name"] | "", sizeof(it.title));
    int year = a["year"] | 0;
    if (year > 0) snprintf(it.subtitle, sizeof(it.subtitle), "%s - %d", (const char *)(a["artist_name"] | ""), year);
    else strlcpy(it.subtitle, a["artist_name"] | "", sizeof(it.subtitle));
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    strlcpy(it.cover, a["artwork"]["proxy_320"] | "", sizeof(it.cover));
    it.favorite = a["favorite"] | false;
}

static void artist_item(JsonObjectConst a, ui::LibItem &it) {
    it.kind = ui::LibKind::Artist;
    it.id   = a["id"] | 0;
    strlcpy(it.title, a["name"] | "", sizeof(it.title));
    int albums = a["album_count"] | 0;
    snprintf(it.subtitle, sizeof(it.subtitle), albums == 1 ? "1 album" : "%d albums", albums);
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    strlcpy(it.cover, a["artwork"]["proxy_320"] | "", sizeof(it.cover));
    it.favorite = a["favorite"] | false;
}

static void playlist_item(JsonObjectConst a, ui::LibItem &it) {
    it.kind = ui::LibKind::Playlist;
    it.id   = a["id"] | 0;
    strlcpy(it.title, a["name"] | "", sizeof(it.title));
    int tracks = a["track_count"] | 0;
    snprintf(it.subtitle, sizeof(it.subtitle), tracks == 1 ? "1 track" : "%d tracks", tracks);
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    strlcpy(it.cover, a["artwork"]["proxy_320"] | "", sizeof(it.cover));
}

static void track_item(JsonObjectConst a, ui::LibItem &it) {
    it.kind     = ui::LibKind::Track;
    it.id       = a["id"] | 0;
    it.album_id = a["album_id"] | 0;
    strlcpy(it.title, a["name"] | "", sizeof(it.title));
    snprintf(it.subtitle, sizeof(it.subtitle), "%s - %s", (const char *)(a["artist_name"] | ""),
             (const char *)(a["album_name"] | ""));
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    strlcpy(it.cover, a["artwork"]["proxy_320"] | "", sizeof(it.cover));
}

static int add_items(JsonArrayConst list, int n, void (*fill)(JsonObjectConst, ui::LibItem &), uint8_t shelf = 0,
                     int max = kMaxItems) {
    int added = 0;
    for (JsonObjectConst o : list) {
        if (n >= kMaxItems || added >= max) break;
        items[n] = {};
        fill(o, items[n]);
        items[n++].shelf = shelf;
        added++;
    }
    return n;
}

static String url_encode(const char *s) {
    String out;
    for (; *s; s++) {
        if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') out += *s;
        else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)*s);
            out += hex;
        }
    }
    return out;
}

static void load_search(const char *query) {
    if (!ensure_buffers()) return;
    JsonDocument res(api::psram());
    String path = String("/library/search?q=") + url_encode(query);
    if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
    int n = add_items(res["artists"], 0, artist_item, 0, 10);
    n     = add_items(res["albums"], n, album_item, 1, 15);
    n     = add_items(res["tracks"], n, track_item, 2, 20);
    ui::library_set_search(query, items, n);
}

// `text`: "" = first page, "more" = next page, "letter:X" = Artists from letter X (A-Z bar)
static void load_library(ui::LibView view, const char *text) {
    bool next_page = strcmp(text, "more") == 0;
    if (!ensure_buffers()) return;
    JsonDocument res(api::psram());
    int n = 0;
    switch (view) {
        case ui::LibView::Recent: {
            // Shelves: recently played, then favorite albums and artists
            if (!api::ok(api::get("/library/recent?limit=20", res))) return ui::library_load_failed();
            n = add_items(res["data"], 0, album_item, 0, 20);
            JsonDocument fav(api::psram());
            if (api::ok(api::get("/library/favorites", fav))) {
                n = add_items(fav["albums"], n, album_item, 1, 20);
                n = add_items(fav["artists"], n, artist_item, 2, 20);
            }
            ui::library_set_items(view, items, n, false, false);
            break;
        }
        case ui::LibView::Favorites:
            if (!api::ok(api::get("/library/favorites", res))) return ui::library_load_failed();
            n = add_items(res["albums"], 0, album_item, 1);  // same shelves as on Recent
            n = add_items(res["artists"], n, artist_item, 2);
            ui::library_set_items(view, items, n, false, false);
            break;
        case ui::LibView::Radio: {
            // Stations the current device can play (GET /devices/{id}/radio)
            if (!model.device()) return ui::library_load_failed();
            String path = device_path(model.device()->id, "radio");
            if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
            for (JsonObjectConst st : res["stations"].as<JsonArrayConst>()) {
                if (n >= kMaxItems) break;
                ui::LibItem &it = items[n++];
                it = {};
                it.kind = ui::LibKind::Station;
                it.id   = st["id"] | 0;
                strlcpy(it.title, st["name"] | "", sizeof(it.title));
                strlcpy(it.thumb, st["artwork"]["proxy_120"] | "", sizeof(it.thumb));
                strlcpy(it.cover, st["artwork"]["proxy_320"] | "", sizeof(it.cover));
            }
            ui::library_set_items(view, items, n, false, false);
            break;
        }
        case ui::LibView::Playlists: {
            if (next_page && playlists_cursor.isEmpty()) return;
            String path = "/library/playlists";
            if (next_page) path += "?cursor=" + playlists_cursor;
            if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
            n = add_items(res["data"], 0, playlist_item);
            JsonVariantConst next = res["next_cursor"];
            playlists_cursor = next.isNull() ? String() : next.as<String>();
            ui::library_set_items(view, items, n, next_page, !playlists_cursor.isEmpty());
            break;
        }
        case ui::LibView::Artists: {
            if (next_page && artists_cursor.isEmpty()) return;
            String path = "/library/artists";
            if (next_page) path += "?cursor=" + artists_cursor;
            else if (strncmp(text, "letter:", 7) == 0 && text[7])
                path += text[7] == '#' ? String("?letter=%23") : String("?letter=") + text[7];
            if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
            n = add_items(res["data"], 0, artist_item);
            JsonVariantConst next = res["next_cursor"];
            artists_cursor = next.isNull() ? String() : next.as<String>();
            ui::library_set_items(view, items, n, next_page, !artists_cursor.isEmpty());
            break;
        }
    }
}

static void load_artist(int artist_id) {
    if (!ensure_buffers()) return;
    JsonDocument res(api::psram());
    String path = String("/library/artists/") + artist_id;
    if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
    int n = add_items(res["albums"], 0, album_item);
    ui::library_set_artist(artist_id, res["name"] | "", res["favorite"] | false, items, n);
}

static void load_album(int album_id) {
    if (!ensure_buffers()) return;
    JsonDocument res(api::psram());
    // With device_id, "playable" means playable on the device we'd send it to
    String path = String("/library/albums/") + album_id;
    if (model.device()) path += "?device_id=" + String(model.device()->id);
    if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();

    ui::LibAlbum album = {};
    album.id = album_id;
    strlcpy(album.name, res["name"] | "", sizeof(album.name));
    album.artist_id = res["artist"]["id"] | 0;
    strlcpy(album.artist, res["artist"]["name"] | "", sizeof(album.artist));
    album.year = res["year"] | 0;
    strlcpy(album.cover, res["artwork"]["proxy_320"] | "", sizeof(album.cover));
    album.favorite = res["favorite"] | false;
    album.playable = res["playable"] | false;

    int n = 0;
    for (JsonObjectConst t : res["tracks"].as<JsonArrayConst>()) {
        if (n >= kMaxTracks) break;
        ui::LibTrack &tr = tracks[n++];
        tr.id = t["id"] | 0;
        strlcpy(tr.name, t["name"] | "", sizeof(tr.name));
        tr.duration = t["duration"] | -1;
        tr.playable = t["playable"] | false;
    }
    ui::library_set_album(album, tracks, n);
}

static void load_playlist(int playlist_id) {
    if (!ensure_buffers()) return;
    JsonDocument res(api::psram());
    String path = String("/library/playlists/") + playlist_id;
    if (model.device()) path += "?device_id=" + String(model.device()->id);
    if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();

    ui::LibAlbum list = {};
    list.id          = playlist_id;
    list.is_playlist = true;
    strlcpy(list.name, res["name"] | "", sizeof(list.name));
    strlcpy(list.artist, res["owner"] | "", sizeof(list.artist));
    strlcpy(list.cover, res["artwork"]["proxy_320"] | "", sizeof(list.cover));
    list.playable = res["playable"] | false;

    int n = 0;
    for (JsonObjectConst t : res["tracks"].as<JsonArrayConst>()) {
        if (n >= kMaxTracks) break;
        ui::LibTrack &tr = tracks[n++];
        tr = {};
        tr.id = t["id"] | 0;
        strlcpy(tr.name, t["name"] | "", sizeof(tr.name));
        strlcpy(tr.artist, t["artist_name"] | "", sizeof(tr.artist));
        tr.duration = t["duration"] | -1;
        tr.playable = t["playable"] | false;
    }
    ui::library_set_album(list, tracks, n);
}

static void play_from_library(Command c) {
    // -1 = whatever device is current when the command runs
    if (c.device_id < 0) {
        if (!model.device()) return ui::toast("No device selected");
        c.device_id = model.device()->id;
    }
    JsonDocument body, res;
    const char *path;
    if (c.type == Type::PlayAlbum) {
        path = "library/play-album";
        body["album_id"] = c.value;
        if (c.value2 > 0) body["start_track_id"] = c.value2;
    } else if (c.type == Type::PlayPlaylist) {
        path = "library/play-playlist";
        body["playlist_id"] = c.value;
        if (c.value2 > 0) body["start_track_id"] = c.value2;
    } else {
        path = "library/play-artist";
        body["artist_id"] = c.value;
        body["shuffle"]   = true;
    }
    if (report(api::post(device_path(c.device_id, path).c_str(), body, res), c.device_id, res)) {
        String msg = String("Playing on ") + device_name(c.device_id);
        ui::toast(msg.c_str());
    }
}

static void set_favorite(const Command &c) {
    JsonDocument body, res;
    body["favorite"] = c.value2 != 0;
    String path = String("/library/") + (c.type == Type::FavoriteAlbum ? "albums/" : "artists/") + c.value + "/favorite";
    int status = api::put(path.c_str(), body, res);
    if (!api::ok(status)) ui::toast(status <= 0 ? "Can't reach the server" : "Couldn't save the favorite");
}

static void load_settings_info() {
    ui::SettingsInfo info = {};
    strlcpy(info.board, board::info().name, sizeof(info.board));
    snprintf(info.firmware, sizeof(info.firmware), "%s (build %d)", FIRMWARE_VERSION, BUILD_NUMBER);
    strlcpy(info.wifi, WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "Not connected", sizeof(info.wifi));
    strlcpy(info.ip, gwNetIP().toString().c_str(), sizeof(info.ip));
    snprintf(info.settings_url, sizeof(info.settings_url), "http://%s", info.ip);
    strlcpy(info.password, settings::web_password_hint().c_str(), sizeof(info.password));
    strlcpy(info.server, model.server_url[0] ? model.server_url : "Not found", sizeof(info.server));
    snprintf(info.mqtt, sizeof(info.mqtt), "%s", model.mqtt_connected ? "Connected" : "Disconnected");
    if (board::info().has_battery) {
        if (model.battery_percent < 0) {
            strlcpy(info.battery, "No battery", sizeof(info.battery));
        } else {
            snprintf(info.battery, sizeof(info.battery), "%d %% (%.2f V)%s", model.battery_percent,
                     model.battery_voltage, model.battery_full ? ", full" : model.battery_usb ? ", charging" : "");
        }
    }
    ui::settings_set_info(info);
}

// ---------------------------------------------------------------------------------

static void run(const Command &c) {
    Serial.printf("[commands] type %d device %d value %d\n", (int)c.type, c.device_id, c.value);
    switch (c.type) {
        case Type::Play:     report(ctl().play(c.device_id), c.device_id); break;
        case Type::Pause:    report(ctl().pause(c.device_id), c.device_id); break;
        case Type::Next:     report(ctl().next(c.device_id), c.device_id); break;
        case Type::Previous: report(ctl().previous(c.device_id), c.device_id); break;
        case Type::Mute:     report(ctl().setMute(c.device_id, c.value), c.device_id); break;
        case Type::Shuffle:  report(ctl().setShuffle(c.device_id, c.value), c.device_id); break;
        case Type::Like:     report(ctl().setLike(c.device_id, c.value), c.device_id); break;
        case Type::Power:    report(ctl().setPower(c.device_id, c.value), c.device_id); break;
        case Type::Repeat:   report(ctl().setRepeat(c.device_id, (Repeat)(c.value & 3)), c.device_id); break;
        case Type::SelectDevice: {
            int i = model.index_of(c.device_id);
            if (i < 0 || i == model.current_index()) break;
            player::client().select(c.device_id);  // remembers it; the bridge refreshes the screens
            player::device_changed();
            break;
        }
        case Type::LoadDetails: load_details(c.device_id); break;
        case Type::LoadSource:  load_source(c.device_id); break;
        case Type::LoadQueue:   load_queue(c.device_id); break;
        case Type::ActivateSource: {
            if (report(ctl().activateSource(c.device_id, c.text), c.device_id)) load_details(c.device_id);
            break;
        }
        case Type::PlayRadio: {
            // -1 = whatever device is current (the library's Radio tab)
            int device_id = c.device_id >= 0 ? c.device_id : (model.device() ? model.device()->id : -1);
            if (device_id < 0) break;
            JsonDocument res;
            if (report(ctl().playRadio(device_id, c.value, res), device_id, res)) {
                String msg = String("Playing ") + (res["station"] | "radio");
                ui::toast(msg.c_str());
            }
            break;
        }
        case Type::JoinGroup: {
            JsonDocument body, res;
            body["host_device_id"] = c.value;
            if (report(api::post(device_path(c.device_id, "multiroom/join").c_str(), body, res), c.device_id, res)) {
                String msg = String("Joined ") + (res["joined"] | "group");
                ui::toast(msg.c_str());
            }
            break;
        }
        case Type::LeaveGroup: {
            JsonDocument res;
            if (report(api::del(device_path(c.device_id, "multiroom/leave").c_str(), res), c.device_id, res))
                ui::toast("Left the group");
            break;
        }
        case Type::LoadLibrary:    load_library((ui::LibView)c.value, c.text); break;
        case Type::LoadSearch:     load_search(c.text); break;
        case Type::Seek:      report(ctl().seek(c.device_id, c.value), c.device_id); break;
        case Type::QueueJump: report(ctl().queueJump(c.device_id, c.value), c.device_id); break;
        case Type::LoadArtist:     load_artist(c.value); break;
        case Type::LoadAlbum:      load_album(c.value); break;
        case Type::LoadPlaylist:   load_playlist(c.value); break;
        case Type::PlayPlaylist:
        case Type::PlayAlbum:
        case Type::PlayArtist:     play_from_library(c); break;
        case Type::FavoriteAlbum:
        case Type::FavoriteArtist: set_favorite(c); break;
        case Type::LoadSettingsInfo: load_settings_info(); break;
        case Type::Volume: break;  // handled below
    }
}

void loop() {
    Command c;
    // One command per loop() pass keeps MQTT and garnet_web responsive
    if (queue && xQueueReceive(queue, &c, 0) == pdTRUE) run(c);

    unsigned long now = millis();
    if (now - last_volume_ms < kVolumeIntervalMs) return;
    portENTER_CRITICAL(&volume_mux);
    int device = volume_device, value = volume_value;
    volume_value = -1;
    portEXIT_CRITICAL(&volume_mux);
    if (value < 0) return;

    last_volume_ms = now;
    report(ctl().setVolume(device, value), device);
}

}  // namespace commands
