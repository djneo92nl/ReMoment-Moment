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

inline bool ok(int status) { return status >= 200 && status < 300; }

}  // namespace api
