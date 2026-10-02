// Minimal blocking JSON client for the ReMoment REST API (http://{server}/api/...).
// Call from the Arduino loop task only.
#pragma once

#include <ArduinoJson.h>

namespace api {

// HTTP status code, or a negative HTTPClient error when the request didn't complete.
// `response` is filled when the body is JSON; it's left empty otherwise.
int get(const char *path, JsonDocument &response);
int post(const char *path, const JsonDocument &body, JsonDocument &response);
int put(const char *path, const JsonDocument &body, JsonDocument &response);
int del(const char *path, JsonDocument &response);

// Same as get(), for an absolute URL (used during discovery, before server_url is known).
int get_url(const char *url, JsonDocument &response);

// Allocator for large responses (library pages, the artwork list): ArduinoJson's pool blocks
// are small enough that malloc puts them in internal RAM, which WiFi and the SD driver need.
// Usage: JsonDocument doc(api::psram());
ArduinoJson::Allocator *psram();

inline bool ok(int status) { return status >= 200 && status < 300; }

}  // namespace api
