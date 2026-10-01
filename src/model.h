// Shared application state. Written only from the Arduino loop task; the LVGL task
// never reads it directly (UI is updated through ui:: calls that take the LVGL lock,
// and UI input goes back through net/commands). garnet_web Info getters read it from
// the HTTP task — they only format text, so a torn read shows a stale value at worst.
#pragma once

#include <stdint.h>

static const uint8_t kMaxDevices = 16;

// Device capabilities as reported by the server (DeviceListResource.capabilities)
enum Capability : uint16_t {
    kCapMediaControls    = 1 << 0,
    kCapVolume           = 1 << 1,
    kCapRadio            = 1 << 2,
    kCapSources          = 1 << 3,
    kCapSourceActivation = 1 << 4,
    kCapMultiroom        = 1 << 5,
    kCapLibraryPlayback  = 1 << 6,
    kCapSeek             = 1 << 7,
    kCapQueue            = 1 << 8,
    kCapShuffle          = 1 << 9,
    kCapRepeat           = 1 << 10,
    kCapLike             = 1 << 11,
    kCapPower            = 1 << 12,
};

enum class PlayState : uint8_t { Unknown, Playing, Paused, Standby, Unreachable };
enum class Repeat : uint8_t { Unknown, Off, All, One };

struct Device {
    int      id;
    char     name[48];
    char     mqtt_topic[48];  // e.g. "remoment/player/4"
    uint16_t caps;

    // Live state from MQTT (all retained topics, so known shortly after subscribing)
    PlayState state = PlayState::Unknown;
    char      track[64]  = "";
    char      artist[64] = "";
    int       progress   = 0;   // 0-100
    unsigned long last_data_ms = 0;
    int       volume  = -1;     // 0-100, -1 = unknown
    bool      muted   = false;
    int8_t    shuffle = -1;     // -1 unknown, 0 off, 1 on
    Repeat    repeat  = Repeat::Unknown;
    int8_t    liked   = -1;

    bool has(uint16_t cap) const { return (caps & cap) == cap; }
    bool active() const { return state == PlayState::Playing || state == PlayState::Paused; }
};

enum class Phase : uint8_t {
    Network,      // waiting for WiFi (or setup portal is open)
    Discovering,  // looking for the ReMoment server
    Registering,  // registered, waiting for admin approval
    Running,      // approved, devices known, MQTT in use
};

struct Model {
    Phase phase = Phase::Network;

    // Server
    char server_url[96] = "";  // e.g. "http://192.168.1.252", no trailing slash
    char artwork_base_url[96] = "";  // prefix for /storage/... artwork paths
    char placeholder_url[192] = "";  // cover shown when there's no artwork (/api/info placeholder)
    char mqtt_host[64]  = "";
    uint16_t mqtt_port  = 1883;
    bool server_ok      = false;  // last API call succeeded
    bool mqtt_connected = false;

    // Registration
    char pairing_code[16] = "";

    // Devices assigned to this client
    Device  devices[kMaxDevices];
    uint8_t device_count  = 0;
    int     current_device = -1;  // index into devices, -1 = none

    bool  sd_available    = false;
    float battery_voltage = 0.0f;

    Device *device() { return current_device >= 0 && current_device < device_count ? &devices[current_device] : nullptr; }
    const Device *device() const {
        return current_device >= 0 && current_device < device_count ? &devices[current_device] : nullptr;
    }
    int index_of(int device_id) const {
        for (int i = 0; i < device_count; i++)
            if (devices[i].id == device_id) return i;
        return -1;
    }
};

extern Model model;
