#include "registration.h"

#include <garnet_web.h>

#include "api.h"
#include "app_config.h"
#include "board/board.h"
#include "model.h"
#include "app_settings.h"

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

static uint16_t parse_capabilities(JsonArrayConst caps) {
    static const struct { const char *name; uint16_t bit; } kMap[] = {
        {"media_controls", kCapMediaControls}, {"volume_control", kCapVolume},
        {"radio_control", kCapRadio},          {"source_control", kCapSources},
        {"source_activation", kCapSourceActivation}, {"multi_room", kCapMultiroom},
        {"library_playback", kCapLibraryPlayback},   {"seek", kCapSeek},
        {"queue", kCapQueue},                  {"shuffle", kCapShuffle},
        {"repeat", kCapRepeat},                {"like", kCapLike},
        {"power", kCapPower},
    };
    uint16_t bits = 0;
    for (const char *c : caps) {
        if (!c) continue;
        for (const auto &m : kMap)
            if (strcmp(c, m.name) == 0) bits |= m.bit;
    }
    return bits;
}

static PlayState parse_state(const char *s) {
    if (!s) return PlayState::Unknown;
    if (strcmp(s, "playing") == 0) return PlayState::Playing;
    if (strcmp(s, "paused") == 0) return PlayState::Paused;
    if (strcmp(s, "standby") == 0) return PlayState::Standby;
    if (strcmp(s, "unreachable") == 0) return PlayState::Unreachable;
    return PlayState::Unknown;
}

static bool load_devices(JsonArrayConst devices) {
    int previous_id = model.device() ? model.device()->id : settings::last_device_id();

    // Rebuild the list, keeping live MQTT state for devices that are still assigned
    static Device fresh[kMaxDevices];
    uint8_t count = 0;
    int current   = -1;
    for (JsonObjectConst d : devices) {
        if (count >= kMaxDevices) break;
        int id = d["id"] | -1;
        if (id < 0) continue;
        int old = model.index_of(id);
        Device &dev = fresh[count];
        dev = old >= 0 ? model.devices[old] : Device{};
        dev.id = id;
        strlcpy(dev.name, d["device_name"] | "", sizeof(dev.name));
        strlcpy(dev.mqtt_topic, d["mqtt_topic"] | "", sizeof(dev.mqtt_topic));
        if (!dev.mqtt_topic[0]) snprintf(dev.mqtt_topic, sizeof(dev.mqtt_topic), "remoment/player/%d", id);
        dev.caps = parse_capabilities(d["capabilities"]);
        if (old < 0) dev.state = parse_state(d["state"]);
        if (id == previous_id) current = count;
        count++;
    }
    memcpy(model.devices, fresh, sizeof(Device) * count);
    model.device_count = count;
    // Server sort order: first assigned device is the default
    model.current_device = current >= 0 ? current : (count > 0 ? 0 : -1);
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
