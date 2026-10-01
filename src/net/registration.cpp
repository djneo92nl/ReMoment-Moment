#include "registration.h"

#include <garnet_web.h>

#include "api.h"
#include "app_config.h"
#include "board/board.h"
#include "model.h"
#include "settings.h"

namespace registration {

static const unsigned long kPollIntervalMs      = 15000;
static const unsigned long kHeartbeatIntervalMs = 10UL * 60 * 1000;

static String        registration_token;
static String        token;
static unsigned long last_attempt_ms   = 0;
static unsigned long last_heartbeat_ms = 0;
static bool          loaded            = false;

static void add_client_info(JsonDocument &body) {
    body["firmware_version"] = FIRMWARE_VERSION;
    body["build_number"]     = BUILD_NUMBER;
}

static bool load_devices(JsonArrayConst devices) {
    int previous_id = model.device() ? model.device()->id : settings::last_device_id();

    model.device_count   = 0;
    model.current_device = -1;
    for (JsonObjectConst d : devices) {
        if (model.device_count >= kMaxDevices) break;
        Device &dev = model.devices[model.device_count];
        dev.id = d["id"] | -1;
        if (dev.id < 0) continue;
        strlcpy(dev.name, d["device_name"] | "", sizeof(dev.name));
        strlcpy(dev.mqtt_topic, d["mqtt_topic"] | "", sizeof(dev.mqtt_topic));
        if (!dev.mqtt_topic[0]) snprintf(dev.mqtt_topic, sizeof(dev.mqtt_topic), "remoment/player/%d", dev.id);
        if (dev.id == previous_id) model.current_device = model.device_count;
        model.device_count++;
    }
    // Server sort order: first assigned device is the default
    if (model.current_device < 0 && model.device_count > 0) model.current_device = 0;
    if (model.device()) settings::set_last_device_id(model.device()->id);
    return true;
}

static Result do_register() {
    JsonDocument body;
    body["hardware_id"] = gwChipId();
    add_client_info(body);
    JsonObject meta     = body["metadata"].to<JsonObject>();
    meta["board"]       = board::info().name;
    meta["screen_w"]    = board::info().width;
    meta["screen_h"]    = board::info().height;
    meta["hostname"]    = gwHostname();

    JsonDocument res;
    int status = api::post("/clients/register", body, res);
    if (!api::ok(status)) return Result::Failed;

    registration_token = res["registration_token"] | "";
    if (registration_token.isEmpty()) return Result::Failed;
    strlcpy(model.pairing_code, res["pairing_code"] | "", sizeof(model.pairing_code));
    settings::set_registration(registration_token, "");
    return Result::Pending;
}

static Result poll_status() {
    JsonDocument res;
    int status = api::get(("/clients/status/" + registration_token).c_str(), res);
    if (status == 404) {  // server forgot us: start over
        registration_token = "";
        settings::clear_registration();
        return Result::Failed;
    }
    if (!api::ok(status)) return Result::Failed;

    if (strcmp(res["status"] | "", "approved") != 0) return Result::Pending;

    token = res["api_token"] | "";
    if (token.isEmpty()) return Result::Failed;
    settings::set_registration(registration_token, token);
    load_devices(res["devices"]);
    model.pairing_code[0] = '\0';
    last_heartbeat_ms = 0;
    return Result::Approved;
}

bool refresh_devices() {
    if (token.isEmpty()) return false;
    JsonDocument res;
    int status = api::get(("/clients/" + token + "/devices").c_str(), res);
    if (status == 404) {  // token revoked: register again
        token = "";
        settings::set_registration(registration_token, "");
        return false;
    }
    return api::ok(status) && load_devices(res["devices"]);
}

Result step() {
    if (!loaded) {
        registration_token = settings::registration_token();
        token              = settings::api_token();
        loaded             = true;
    }

    unsigned long now = millis();
    if (last_attempt_ms != 0 && now - last_attempt_ms < kPollIntervalMs) {
        return registration_token.isEmpty() ? Result::Failed : Result::Pending;
    }
    last_attempt_ms = now;

    if (!token.isEmpty()) {
        if (refresh_devices()) return Result::Approved;
        if (!token.isEmpty()) return Result::Failed;  // network error, token still valid
    }
    // Re-registering a known hardware_id is idempotent and refreshes the pairing code
    if (registration_token.isEmpty() || !model.pairing_code[0]) {
        Result r = do_register();
        if (r != Result::Pending) return r;
    }
    return poll_status();
}

void heartbeat_loop() {
    if (token.isEmpty()) return;
    unsigned long now = millis();
    if (last_heartbeat_ms != 0 && now - last_heartbeat_ms < kHeartbeatIntervalMs) return;
    last_heartbeat_ms = now;

    JsonDocument body, res;
    add_client_info(body);
    api::put(("/clients/" + token + "/heartbeat").c_str(), body, res);
}

String api_token() { return token; }

void reset() {
    loaded             = false;
    registration_token = "";
    token              = "";
    last_attempt_ms    = 0;
    last_heartbeat_ms  = 0;
    model.pairing_code[0] = '\0';
    model.device_count    = 0;
    model.current_device  = -1;
}

}  // namespace registration
