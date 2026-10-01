#include "player.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <garnet_web.h>

#include "artwork/artwork.h"
#include "model.h"
#include "ui/ui.h"

namespace player {

static const unsigned long kNotPlayingTimeoutMs     = 15000;
static const unsigned long kMqttReconnectIntervalMs = 10000;

static WiFiClient   wifi_client;
static PubSubClient mqtt(wifi_client);

static unsigned long last_attempt_ms = 0;

// Subscribed topic filter per device ("remoment/player/4/#"), to unsubscribe after a list change
static char subscribed[kMaxDevices][sizeof(Device::mqtt_topic) + 2];
static uint8_t subscribed_count = 0;

// Latest artwork per device, so switching devices shows the right cover at once
static artwork::Request device_artwork[kMaxDevices];

// "#rrggbb" -> 0xRRGGBB
static bool parse_hex_color(const char *s, uint32_t &out) {
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    char *end;
    out = strtoul(s + 1, &end, 16);
    return *end == '\0';
}

static void parse_artwork(JsonObjectConst art, artwork::Request &req) {
    req = {};
    strlcpy(req.cover_url, art["proxy_320"] | "", sizeof(req.cover_url));
    strlcpy(req.background_url, art["proxy_bg"] | "", sizeof(req.background_url));
    req.has_accent = parse_hex_color(art["safe_colors"][0] | "", req.accent);
}

static int8_t parse_tristate(JsonVariantConst v) { return v.isNull() ? -1 : (v.as<bool>() ? 1 : 0); }

static Repeat parse_repeat(const char *s) {
    if (!s) return Repeat::Unknown;
    if (strcmp(s, "off") == 0) return Repeat::Off;
    if (strcmp(s, "all") == 0) return Repeat::All;
    if (strcmp(s, "one") == 0) return Repeat::One;
    return Repeat::Unknown;
}

static PlayState parse_state(const char *s) {
    if (!s) return PlayState::Unknown;
    if (strcmp(s, "playing") == 0) return PlayState::Playing;
    if (strcmp(s, "paused") == 0) return PlayState::Paused;
    if (strcmp(s, "standby") == 0) return PlayState::Standby;
    if (strcmp(s, "unreachable") == 0) return PlayState::Unreachable;
    return PlayState::Unknown;
}

// Pushes a device change to the screens that show it
static void changed(int index, uint8_t what) {
    const Device &d = model.devices[index];
    if (index == model.current_device) ui::now_playing_update(d, what);
    ui::devices_update(index, d);
}

static void clear_track(Device &d) {
    d.track[0] = d.artist[0] = '\0';
    d.progress = 0;
}

static void on_message(char *topic, byte *payload, unsigned int length) {
    // Find the device whose base topic prefixes this one
    int index = -1;
    const char *suffix = nullptr;
    for (int i = 0; i < model.device_count; i++) {
        size_t n = strlen(model.devices[i].mqtt_topic);
        if (strncmp(topic, model.devices[i].mqtt_topic, n) == 0 && topic[n] == '/') {
            index  = i;
            suffix = topic + n + 1;
            break;
        }
    }
    if (index < 0) return;
    Device &d    = model.devices[index];
    bool current = index == model.current_device;

    if (strcmp(suffix, "progress") == 0) {
        char value[8];
        if (length >= sizeof(value)) return;
        memcpy(value, payload, length);
        value[length]  = '\0';
        d.progress     = constrain(atoi(value), 0, 100);
        d.last_data_ms = millis();
        if (current) ui::now_playing_update(d, ui::kUpdateProgress);
        return;
    }

    if (strcmp(suffix, "data") == 0) {
        // Empty (retained-clear) payload = nothing playing on this device
        if (length == 0) {
            clear_track(d);
            device_artwork[index] = {};
        } else {
            JsonDocument doc;
            if (deserializeJson(doc, payload, length)) return;
            strlcpy(d.artist, doc["artist"] | "", sizeof(d.artist));
            strlcpy(d.track, doc["track"] | "", sizeof(d.track));
            // No artwork object yet on the first play of a new cover (the server processes it async)
            parse_artwork(doc["artwork"], device_artwork[index]);
            d.last_data_ms = millis();
        }
        if (current) artwork::request(device_artwork[index]);
        changed(index, ui::kUpdateTrack);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) return;

    if (strcmp(suffix, "state") == 0) {
        d.state = parse_state(doc["state"]);
        if (d.state == PlayState::Playing) d.last_data_ms = millis();
        if (!d.active()) clear_track(d);
        changed(index, ui::kUpdateState | ui::kUpdateTrack);
    } else if (strcmp(suffix, "volume") == 0) {
        d.volume = doc["volume"] | d.volume;
        d.muted  = doc["muted"] | d.muted;
        if (current) ui::now_playing_update(d, ui::kUpdateVolume);
    } else if (strcmp(suffix, "modes") == 0) {
        d.shuffle = parse_tristate(doc["shuffle"]);
        d.repeat  = parse_repeat(doc["repeat"]);
        d.liked   = parse_tristate(doc["liked"]);
        if (current) ui::now_playing_update(d, ui::kUpdateModes);
    }
}

// Subscribes to everything of every assigned device (state and track feed the device list)
static void resubscribe() {
    if (!mqtt.connected()) return;
    for (uint8_t i = 0; i < subscribed_count; i++) mqtt.unsubscribe(subscribed[i]);
    subscribed_count = 0;
    for (uint8_t i = 0; i < model.device_count; i++) {
        snprintf(subscribed[i], sizeof(subscribed[i]), "%s/#", model.devices[i].mqtt_topic);
        mqtt.subscribe(subscribed[i]);
        subscribed_count++;
    }
}

static void connect() {
    if (mqtt.connected() || !model.mqtt_host[0]) return;
    unsigned long now = millis();
    if (last_attempt_ms != 0 && now - last_attempt_ms < kMqttReconnectIntervalMs) return;
    last_attempt_ms = now;

    mqtt.setServer(model.mqtt_host, model.mqtt_port);
    String client_id = "remoment-client-" + gwChipId();
    if (mqtt.connect(client_id.c_str())) {
        Serial.println("[mqtt] connected");
        subscribed_count = 0;
        resubscribe();
    } else {
        Serial.printf("[mqtt] connect to %s:%u failed, rc=%d\n", model.mqtt_host, model.mqtt_port, mqtt.state());
    }
}

void begin() {
    mqtt.setCallback(on_message);
    mqtt.setSocketTimeout(3);
    mqtt.setBufferSize(1024);  // /data carries artwork URLs and colors
}

void devices_changed() {
    resubscribe();
    ui::devices_sync(model.devices, model.device_count, model.current_device);
}

void device_changed() {
    const Device *d = model.device();
    if (d) {
        ui::now_playing_update(*d, ui::kUpdateAll);
        artwork::request(device_artwork[model.current_device]);
    } else {
        artwork::clear();
    }
    ui::devices_sync(model.devices, model.device_count, model.current_device);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        model.mqtt_connected = false;
        return;
    }
    connect();
    mqtt.loop();
    model.mqtt_connected = mqtt.connected();

    // Fallback when the server goes quiet: no progress for a while = not playing
    unsigned long now = millis();
    for (int i = 0; i < model.device_count; i++) {
        Device &d = model.devices[i];
        if (d.state == PlayState::Playing && now - d.last_data_ms > kNotPlayingTimeoutMs) {
            d.state = PlayState::Unknown;
            changed(i, ui::kUpdateState);
        }
    }
}

}  // namespace player
