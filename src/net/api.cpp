#include "api.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include "model.h"

namespace api {

static const uint16_t kTimeoutMs = 5000;

static int request(const char *method, const String &url, const JsonDocument *body, JsonDocument &response) {
    response.clear();
    if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_NOT_CONNECTED;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kTimeoutMs);
    http.setConnectTimeout(kTimeoutMs);
    if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
    http.addHeader("Accept", "application/json");

    int status;
    if (body) {
        String payload;
        serializeJson(*body, payload);
        http.addHeader("Content-Type", "application/json");
        status = http.sendRequest(method, payload);
    } else {
        status = http.sendRequest(method);
    }

    if (status > 0) {
        String payload = http.getString();
        if (payload.length() > 0 && deserializeJson(response, payload)) response.clear();
    }
    http.end();

    // Only connection-level failures say anything about server reachability
    if (model.server_url[0] && url.startsWith(model.server_url)) model.server_ok = status > 0 && status < 500;
    if (status <= 0) Serial.printf("[api] %s %s failed: %s\n", method, url.c_str(), HTTPClient::errorToString(status).c_str());
    return status;
}

static String api_url(const char *path) { return String(model.server_url) + "/api" + path; }

int get(const char *path, JsonDocument &response) { return request("GET", api_url(path), nullptr, response); }

int post(const char *path, const JsonDocument &body, JsonDocument &response) {
    return request("POST", api_url(path), &body, response);
}

int put(const char *path, const JsonDocument &body, JsonDocument &response) {
    return request("PUT", api_url(path), &body, response);
}

int del(const char *path, JsonDocument &response) { return request("DELETE", api_url(path), nullptr, response); }

int get_url(const char *url, JsonDocument &response) { return request("GET", url, nullptr, response); }

}  // namespace api
