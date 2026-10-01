#include "commands.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "api.h"
#include "model.h"
#include "player.h"
#include "settings.h"
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
    return i >= 0 ? model.devices[i].name : "the device";
}

// Turns an API failure into a short message for the toast
static bool report(int status, int device_id, const JsonDocument &res) {
    if (api::ok(status)) return true;
    char msg[96];
    if (status <= 0) {
        snprintf(msg, sizeof(msg), "Can't reach the server");
    } else if (status == 503) {
        snprintf(msg, sizeof(msg), "%s is not reachable", device_name(device_id));
    } else if (status == 422) {
        snprintf(msg, sizeof(msg), "%s can't do that", device_name(device_id));
    } else {
        const char *m = res["message"] | "";
        snprintf(msg, sizeof(msg), "%s", *m ? m : "Something went wrong");
    }
    ui::toast(msg);
    return false;
}

static void put_value(int device_id, const char *what, const char *key, JsonVariantConst value) {
    JsonDocument body, res;
    body[key] = value;
    report(api::put(device_path(device_id, what).c_str(), body, res), device_id, res);
}

static void put_bool(int device_id, const char *what, const char *key, bool value) {
    JsonDocument v;
    v.set(value);
    put_value(device_id, what, key, v.as<JsonVariantConst>());
}

static void post_action(int device_id, const char *action) {
    JsonDocument body, res;
    report(api::post(device_path(device_id, action).c_str(), body, res), device_id, res);
}

// ---- Details screen data ---------------------------------------------------------

static const int kMaxOptions = 32;

static void load_details(int device_id) {
    int i = model.index_of(device_id);
    if (i < 0) return;
    const Device &d = model.devices[i];
    static ui::Option options[kMaxOptions];

    if (d.has(kCapSourceActivation)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(api::get(device_path(device_id, "sources").c_str(), res))) {
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
        if (api::ok(api::get(device_path(device_id, "radio").c_str(), res))) {
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
static String        artists_cursor;  // next page of the artist list, "" = none

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
    if (year > 0) snprintf(it.subtitle, sizeof(it.subtitle), "%s  \xC2\xB7  %d", (const char *)(a["artist_name"] | ""), year);
    else strlcpy(it.subtitle, a["artist_name"] | "", sizeof(it.subtitle));
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    it.favorite = a["favorite"] | false;
}

static void artist_item(JsonObjectConst a, ui::LibItem &it) {
    it.kind = ui::LibKind::Artist;
    it.id   = a["id"] | 0;
    strlcpy(it.title, a["name"] | "", sizeof(it.title));
    int albums = a["album_count"] | 0;
    snprintf(it.subtitle, sizeof(it.subtitle), albums == 1 ? "1 album" : "%d albums", albums);
    strlcpy(it.thumb, a["artwork"]["proxy_120"] | "", sizeof(it.thumb));
    it.favorite = a["favorite"] | false;
}

static int add_items(JsonArrayConst list, int n, void (*fill)(JsonObjectConst, ui::LibItem &)) {
    for (JsonObjectConst o : list) {
        if (n >= kMaxItems) break;
        items[n] = {};
        fill(o, items[n++]);
    }
    return n;
}

static void load_library(ui::LibView view, bool next_page) {
    if (!ensure_buffers()) return;
    JsonDocument res;
    int n = 0;
    switch (view) {
        case ui::LibView::Recent:
            if (!api::ok(api::get("/library/recent?limit=48", res))) return ui::library_load_failed();
            n = add_items(res["data"], 0, album_item);
            ui::library_set_items(view, items, n, false, false);
            break;
        case ui::LibView::Favorites:
            if (!api::ok(api::get("/library/favorites", res))) return ui::library_load_failed();
            n = add_items(res["albums"], 0, album_item);
            n = add_items(res["artists"], n, artist_item);
            ui::library_set_items(view, items, n, false, false);
            break;
        case ui::LibView::Artists: {
            if (next_page && artists_cursor.isEmpty()) return;
            String path = "/library/artists";
            if (next_page) path += "?cursor=" + artists_cursor;
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
    JsonDocument res;
    String path = String("/library/artists/") + artist_id;
    if (!api::ok(api::get(path.c_str(), res))) return ui::library_load_failed();
    int n = add_items(res["albums"], 0, album_item);
    ui::library_set_artist(artist_id, res["name"] | "", res["favorite"] | false, items, n);
}

static void load_album(int album_id) {
    if (!ensure_buffers()) return;
    JsonDocument res;
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

// ---------------------------------------------------------------------------------

static void run(const Command &c) {
    switch (c.type) {
        case Type::Play:     post_action(c.device_id, "play"); break;
        case Type::Pause:    post_action(c.device_id, "pause"); break;
        case Type::Next:     post_action(c.device_id, "next"); break;
        case Type::Previous: post_action(c.device_id, "previous"); break;
        case Type::Mute:     put_bool(c.device_id, "mute", "muted", c.value); break;
        case Type::Shuffle:  put_bool(c.device_id, "shuffle", "shuffle", c.value); break;
        case Type::Like:     put_bool(c.device_id, "like", "liked", c.value); break;
        case Type::Power:    put_bool(c.device_id, "power", "on", c.value); break;
        case Type::Repeat: {
            static const char *kModes[] = {"off", "off", "all", "one"};  // indexed by Repeat
            JsonDocument v;
            v.set(kModes[c.value & 3]);
            put_value(c.device_id, "repeat", "repeat", v.as<JsonVariantConst>());
            break;
        }
        case Type::SelectDevice: {
            int i = model.index_of(c.device_id);
            if (i < 0 || i == model.current_device) break;
            model.current_device = i;
            settings::set_last_device_id(c.device_id);
            player::device_changed();
            break;
        }
        case Type::LoadDetails: load_details(c.device_id); break;
        case Type::ActivateSource: {
            JsonDocument body, res;
            body["source_id"] = c.text;
            if (report(api::post(device_path(c.device_id, "sources/activate").c_str(), body, res), c.device_id, res))
                load_details(c.device_id);
            break;
        }
        case Type::PlayRadio: {
            JsonDocument body, res;
            String path = device_path(c.device_id, "radio/") + c.value;
            if (report(api::post(path.c_str(), body, res), c.device_id, res)) {
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
        case Type::LoadLibrary:    load_library((ui::LibView)c.value, c.text[0] != '\0'); break;
        case Type::LoadArtist:     load_artist(c.value); break;
        case Type::LoadAlbum:      load_album(c.value); break;
        case Type::PlayAlbum:
        case Type::PlayArtist:     play_from_library(c); break;
        case Type::FavoriteAlbum:
        case Type::FavoriteArtist: set_favorite(c); break;
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
    JsonDocument v;
    v.set(value);
    put_value(device, "volume", "volume", v.as<JsonVariantConst>());
}

}  // namespace commands
