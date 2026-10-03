// garnet_web settings groups and the persisted registration state.
#pragma once

#include <Arduino.h>

namespace settings {

// Registers the app's groups with garnet_settings; call before gwBegin().
void register_groups();

// Server URL from the web UI, "" = discover via mDNS
String server_url();

// Registration state (not user-editable, kept out of the web UI)
String registration_token();
String api_token();
void   set_registration(const String &registration_token, const String &api_token);
void   clear_registration();

int  last_device_id();
void set_last_device_id(int id);

// Set from the web UI's "Re-register" / "Find server again" buttons; main loop consumes them.
bool take_rediscover_request();

// Display and sound values, 0-100 (shared by the web UI and the device's settings screen)
enum class Value : uint8_t { Brightness, LedBrightness, TapVolume };
int value(Value v);

// Battery low: caps the backlight at 30 % and turns the LED off; the stored settings are untouched.
// Safe from any task.
void set_low_power(bool on);

// Applies a value right away; safe from any task (the device's settings screen)
void preview(Value v, int percent);

// Writes previewed values to NVS once they stopped changing; call from the loop task.
void save_if_settled();

// Seconds without a touch before Now Playing goes calm (controls hidden), 0 = never.
// Safe from any task.
int calm_after_s();

// "Settings password: ..." — the real one only while it's still the firmware default
// (garnet_web keeps just a salted hash once it's changed)
String web_password_hint();

}  // namespace settings
