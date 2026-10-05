#include "player.h"

#include <WiFi.h>
#include <garnet_web.h>

#include "app_config.h"
#include "app_settings.h"
#include "artwork/artwork.h"
#include "board/board.h"
#include "model.h"
#include "ui/ui.h"

namespace player {

static remoment::Client             rm;
static remoment::PreferencesStorage storage("rm_reg");  // same keys the app always used
static char                         server_url[96];
static char                         chip_id[24];
static char                         hostname[48];
static char                         background_key[24];

remoment::Client &client() { return rm; }

// The LED wants a hue, not the readable-on-black safe colours (those are lightened and look
// white on an LED). Take the most saturated raw palette colour at full brightness, with its
// saturation pushed up: album palettes are often muted, and a pastel reads as white on an
// LED. Grey palettes (hardly any hue) stay a soft white.
static uint32_t led_color(const uint32_t *colors, uint8_t count) {
    uint32_t best = 0;
    float best_sat = -1;
    for (uint8_t i = 0; i < count; i++) {
        uint32_t c = colors[i];
        float r = ((c >> 16) & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
        float mx = max(r, max(g, b)), mn = min(r, min(g, b));
        float sat = mx > 0 ? (mx - mn) / mx : 0;
        if (sat > best_sat) {
            best_sat = sat;
            best     = c;
        }
    }
    float r = ((best >> 16) & 0xFF) / 255.0f, g = ((best >> 8) & 0xFF) / 255.0f, b = (best & 0xFF) / 255.0f;
    float mx = max(r, max(g, b)), mn = min(r, min(g, b));
    if (mx <= 0) return 0;

    // HSV with V = 1 and a stronger S, same hue
    float sat = (mx - mn) / mx;
    float boosted = sat < 0.12f ? sat : min(0.8f, sat * 1.2f + 0.1f);
    auto channel = [&](float v) {
        float norm = mx > mn ? (v - mn) / (mx - mn) : 1.0f;  // 0 at the weakest channel, 1 at the strongest
        return (uint32_t)(255.0f * (1.0f - boosted * (1.0f - norm)));
    };
    return channel(r) << 16 | channel(g) << 8 | channel(b);
}

// What the artwork task needs for a device's current track
static artwork::Request artwork_request(const Device &d) {
    artwork::Request req = {};
    strlcpy(req.cover_url, d.artwork.cover_url, sizeof(req.cover_url));
    strlcpy(req.background_url, d.artwork.background_url, sizeof(req.background_url));
    req.has_accent = d.artwork.has_accent;
    req.accent     = d.artwork.accent;
    req.led        = led_color(d.artwork.colors, d.artwork.color_count);
    return req;
}

// Status LED: the current album's colour while something plays
static void update_led() {
    const Device *d = model.device();
    board::set_led(d && d->active() ? led_color(d->artwork.colors, d->artwork.color_count) : 0);
}

static uint8_t ui_flags(uint8_t changes) {
    uint8_t what = 0;
    if (changes & remoment::kChangeTrack)    what |= ui::kUpdateTrack;
    if (changes & remoment::kChangeProgress) what |= ui::kUpdateProgress;
    if (changes & remoment::kChangeState)    what |= ui::kUpdateState;
    if (changes & remoment::kChangeVolume)   what |= ui::kUpdateVolume;
    if (changes & remoment::kChangeModes)    what |= ui::kUpdateModes;
    return what;
}

static void sync_devices() {
    // The UI keeps its own copy of the list: hand it the library's devices in order
    ui::devices_sync(rm.device(0), rm.deviceCount(), rm.currentIndex());
}

class Bridge : public remoment::Listener {
    int last_device_id = -1;

    void onPhase(remoment::Phase phase) override { model.phase = phase; }

    void onDevicesChanged() override {
        const Device *d = rm.current();
        int id = d ? d->id : -1;
        sync_devices();
        if (id != last_device_id) {
            last_device_id = id;
            device_changed();
        }
    }

    void onDeviceChanged(uint8_t index, uint8_t changes) override {
        const Device &d = *rm.device(index);
        bool current = index == rm.currentIndex();
        uint8_t what = ui_flags(changes);

        // A cover change on the current device; a state change only clears the text, the cover stays
        if (current && (changes & remoment::kChangeTrack) && !(changes & remoment::kChangeState))
            artwork::request(artwork_request(d));

        if (changes & (remoment::kChangeTrack | remoment::kChangeState)) {
            if (current) {
                ui::now_playing_update(d, what);
                update_led();
            }
            ui::devices_update(index, d);
        } else if (current) {
            ui::now_playing_update(d, what);
        }
    }
} bridge;

static bool network_up() {
    GwNetMode mode = gwNetMode();
    return mode == GwNetMode::WiFi || mode == GwNetMode::Ethernet;
}

static void load_server_url() { strlcpy(server_url, settings::server_url().c_str(), sizeof(server_url)); }

void begin() {
    strlcpy(chip_id, gwChipId().c_str(), sizeof(chip_id));
    strlcpy(hostname, gwHostname().c_str(), sizeof(hostname));
    snprintf(background_key, sizeof(background_key), "proxy_bg_%ux%u", board::info().width, board::info().height);
    load_server_url();

    remoment::Config cfg;
    cfg.info.hardware_id      = chip_id;
    cfg.info.board            = board::info().name;
    cfg.info.hostname         = hostname;
    cfg.info.firmware_version = FIRMWARE_VERSION;
    cfg.info.build_number     = BUILD_NUMBER;
    cfg.info.screen_w         = board::info().width;
    cfg.info.screen_h         = board::info().height;
    cfg.server_url            = server_url;
    cfg.storage               = &storage;
    cfg.listener              = &bridge;
    cfg.network_up            = network_up;
    cfg.background_key        = background_key;
    rm.begin(cfg);
}

void device_changed() {
    const Device *d = model.device();
    if (d) {
        ui::now_playing_update(*d, ui::kUpdateAll);
        artwork::request(artwork_request(*d));
    } else {
        artwork::clear();
    }
    sync_devices();
    update_led();
}

bool has_devices() { return rm.deviceCount() > 0; }

void restart() {
    load_server_url();
    rm.restart();
}

void forget_registration() {
    load_server_url();
    rm.forgetRegistration();
}

void loop() {
    rm.loop();

    // Mirror what the settings page, the artwork cache and the status screens read
    const remoment::protocol::ServerInfo &info = rm.server();
    strlcpy(model.server_url, rm.http().server(), sizeof(model.server_url));
    strlcpy(model.mqtt_host, info.mqtt_host, sizeof(model.mqtt_host));
    model.mqtt_port = info.mqtt_port;
    strlcpy(model.artwork_base_url, info.artwork_base_url, sizeof(model.artwork_base_url));
    strlcpy(model.placeholder_url, info.placeholder_url, sizeof(model.placeholder_url));
    strlcpy(model.pairing_code, rm.pairingCode(), sizeof(model.pairing_code));
    model.server_ok      = rm.serverReachable();
    model.mqtt_connected = rm.mqttConnected();
}

}  // namespace player

// ---- Model accessors over the library's device list ----------------------------------

uint8_t Model::device_count() const { return player::client().deviceCount(); }
Device *Model::device() { return player::client().current(); }
const Device *Model::device() const { return player::client().current(); }
Device *Model::device_at(uint8_t index) { return player::client().device(index); }
int Model::current_index() const { return player::client().currentIndex(); }
int Model::index_of(int device_id) const { return player::client().indexOf(device_id); }
