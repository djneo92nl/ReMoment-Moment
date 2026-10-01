// Shared application state. Written only from the Arduino loop task; the LVGL task
// never reads it directly (UI is updated through ui:: calls that take the LVGL lock).
// garnet_web Info getters read it from the HTTP task — they only format text, so a
// torn read shows a stale value at worst.
#pragma once

#include <stdint.h>

static const uint8_t kMaxDevices = 16;

struct Device {
    int  id;
    char name[48];
    char mqtt_topic[48];  // e.g. "remoment/player/4"
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

    // Now playing (current device)
    char track_artist[64] = "";
    char track_name[64]   = "";
    int  track_progress   = 0;  // 0-100
    bool is_playing       = false;
    unsigned long last_data_ms = 0;

    bool  sd_available    = false;
    float battery_voltage = 0.0f;

    const Device *device() const {
        return current_device >= 0 && current_device < device_count ? &devices[current_device] : nullptr;
    }
};

extern Model model;
