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

}  // namespace settings
