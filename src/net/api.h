// Thin wrappers over the ReMoment library's HTTP client (player::client().http()), so the
// command and artwork code keep their short call sites. Blocking; the library client is not
// thread-safe for its own state, but requests themselves share nothing (the artwork task
// uses them too).
#pragma once

#include <ArduinoJson.h>
#include <remoment/http.h>

namespace api {

int get(const char *path, JsonDocument &response);
int post(const char *path, const JsonDocument &body, JsonDocument &response);
int put(const char *path, const JsonDocument &body, JsonDocument &response);
int del(const char *path, JsonDocument &response);

// Allocator for large responses (library pages, the artwork list): ArduinoJson's pool blocks
// are small enough that malloc puts them in internal RAM, which WiFi and the SD driver need.
// Usage: JsonDocument doc(api::psram());
ArduinoJson::Allocator *psram();

inline bool ok(int status) { return remoment::httpOk(status); }

}  // namespace api
