// Shared application state. Written only from the Arduino loop task; the LVGL task
// never reads it directly (UI is updated through ui:: calls that take the LVGL lock,
// and UI input goes back through net/commands). garnet_web Info getters read it from
// the HTTP task — they only format text, so a torn read shows a stale value at worst.
#pragma once

#include <stdint.h>

// Protocol types come from the ReMoment library (lib: ReMomentLib); the UI keeps using these names
#include <remoment/types.h>

using remoment::Capability;
using remoment::Device;
using remoment::Phase;
using remoment::PlayState;
using remoment::Repeat;
using remoment::kCapLibraryPlayback;
using remoment::kCapLike;
using remoment::kCapMediaControls;
using remoment::kCapMultiroom;
using remoment::kCapPower;
using remoment::kCapQueue;
using remoment::kCapQueueJump;
using remoment::kCapRadio;
using remoment::kCapRepeat;
using remoment::kCapSeek;
using remoment::kCapShuffle;
using remoment::kCapSourceActivation;
using remoment::kCapSources;
using remoment::kCapVolume;

static const uint8_t kMaxDevices = RM_MAX_DEVICES;

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

    // Devices assigned to this client live in the library client (net/player.cpp)
    uint8_t       device_count() const;
    Device       *device();
    const Device *device() const;
    Device       *device_at(uint8_t index);
    int           current_index() const;
    int           index_of(int device_id) const;

    bool  sd_available    = false;
    float battery_voltage = 0.0f;
    int   battery_percent = -1;     // -1 = no battery detected
    bool  battery_usb     = false;  // external power present
    bool  battery_full    = false;
};

extern Model model;
