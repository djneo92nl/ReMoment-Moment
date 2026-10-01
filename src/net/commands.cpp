#include "commands.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "api.h"
#include "model.h"
#include "player.h"
#include "settings.h"
#include "ui/ui.h"

namespace commands {

static const unsigned long kVolumeIntervalMs = 200;  // max rate while dragging the slider

static QueueHandle_t queue;

// Volume is coalesced outside the queue: dragging produces many values, only the last counts
static portMUX_TYPE volume_mux = portMUX_INITIALIZER_UNLOCKED;
static int volume_device       = -1;
static int volume_value        = -1;
static unsigned long last_volume_ms = 0;

void begin() { queue = xQueueCreate(16, sizeof(Command)); }

void post(Type type, int device_id, int value, const char *text) {
    if (type == Type::Volume) {
        portENTER_CRITICAL(&volume_mux);
        volume_device = device_id;
        volume_value  = value;
        portEXIT_CRITICAL(&volume_mux);
        return;
    }
    Command c = {type, device_id, value, ""};
    if (text) strlcpy(c.text, text, sizeof(c.text));
    if (queue && xQueueSend(queue, &c, 0) != pdTRUE) Serial.println("[commands] queue full, dropped");
}

static String device_path(int device_id, const char *suffix) {
    return String("/devices/") + device_id + "/" + suffix;
}

static const char *device_name(int device_id) {
    int i = model.index_of(device_id);
    return i >= 0 ? model.devices[i].name : "the device";
}

// Turns an API failure into a short message for the toast
static bool report(int status, int device_id, const JsonDocument &res) {
    if (api::ok(status)) return true;
    char msg[96];
    if (status <= 0) {
        snprintf(msg, sizeof(msg), "Can't reach the server");
    } else if (status == 503) {
        snprintf(msg, sizeof(msg), "%s is not reachable", device_name(device_id));
    } else if (status == 422) {
        snprintf(msg, sizeof(msg), "%s can't do that", device_name(device_id));
    } else {
        const char *m = res["message"] | "";
        snprintf(msg, sizeof(msg), "%s", *m ? m : "Something went wrong");
    }
    ui::toast(msg);
    return false;
}

static void put_value(int device_id, const char *what, const char *key, JsonVariantConst value) {
    JsonDocument body, res;
    body[key] = value;
    report(api::put(device_path(device_id, what).c_str(), body, res), device_id, res);
}

static void put_bool(int device_id, const char *what, const char *key, bool value) {
    JsonDocument v;
    v.set(value);
    put_value(device_id, what, key, v.as<JsonVariantConst>());
}

static void post_action(int device_id, const char *action) {
    JsonDocument body, res;
    report(api::post(device_path(device_id, action).c_str(), body, res), device_id, res);
}

// ---- Details screen data ---------------------------------------------------------

static const int kMaxOptions = 32;

static void load_details(int device_id) {
    int i = model.index_of(device_id);
    if (i < 0) return;
    const Device &d = model.devices[i];
    static ui::Option options[kMaxOptions];

    if (d.has(kCapSourceActivation)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(api::get(device_path(device_id, "sources").c_str(), res))) {
            for (JsonObjectConst s : res["sources"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                strlcpy(o.id, s["source_id"] | "", sizeof(o.id));
                strlcpy(o.label, s["friendly_name"] | o.id, sizeof(o.label));
                strlcpy(o.detail, s["provider_name"] | "", sizeof(o.detail));
                o.active = s["in_use"] | false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Sources, options, n);
    }

    if (d.has(kCapRadio)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(api::get(device_path(device_id, "radio").c_str(), res))) {
            for (JsonObjectConst s : res["stations"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                snprintf(o.id, sizeof(o.id), "%d", s["id"] | 0);
                strlcpy(o.label, s["name"] | "", sizeof(o.label));
                strlcpy(o.detail, s["genre"] | "", sizeof(o.detail));
                o.active = false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Radio, options, n);
    }

    if (d.has(kCapMultiroom)) {
        JsonDocument res;
        int n = 0;
        if (api::ok(api::get(device_path(device_id, "multiroom").c_str(), res))) {
            for (JsonObjectConst s : res["joinable"].as<JsonArrayConst>()) {
                if (n >= kMaxOptions) break;
                ui::Option &o = options[n++];
                snprintf(o.id, sizeof(o.id), "%d", s["id"] | 0);
                strlcpy(o.label, s["device_name"] | "", sizeof(o.label));
                strlcpy(o.detail, s["state"] | "", sizeof(o.detail));
                o.active = false;
            }
        }
        ui::details_set_options(device_id, ui::Section::Group, options, n);
    }
}

// ---------------------------------------------------------------------------------

static void run(const Command &c) {
    switch (c.type) {
        case Type::Play:     post_action(c.device_id, "play"); break;
        case Type::Pause:    post_action(c.device_id, "pause"); break;
        case Type::Next:     post_action(c.device_id, "next"); break;
        case Type::Previous: post_action(c.device_id, "previous"); break;
        case Type::Mute:     put_bool(c.device_id, "mute", "muted", c.value); break;
        case Type::Shuffle:  put_bool(c.device_id, "shuffle", "shuffle", c.value); break;
        case Type::Like:     put_bool(c.device_id, "like", "liked", c.value); break;
        case Type::Power:    put_bool(c.device_id, "power", "on", c.value); break;
        case Type::Repeat: {
            static const char *kModes[] = {"off", "off", "all", "one"};  // indexed by Repeat
            JsonDocument v;
            v.set(kModes[c.value & 3]);
            put_value(c.device_id, "repeat", "repeat", v.as<JsonVariantConst>());
            break;
        }
        case Type::SelectDevice: {
            int i = model.index_of(c.device_id);
            if (i < 0 || i == model.current_device) break;
            model.current_device = i;
            settings::set_last_device_id(c.device_id);
            player::device_changed();
            break;
        }
        case Type::LoadDetails: load_details(c.device_id); break;
        case Type::ActivateSource: {
            JsonDocument body, res;
            body["source_id"] = c.text;
            if (report(api::post(device_path(c.device_id, "sources/activate").c_str(), body, res), c.device_id, res))
                load_details(c.device_id);
            break;
        }
        case Type::PlayRadio: {
            JsonDocument body, res;
            String path = device_path(c.device_id, "radio/") + c.value;
            if (report(api::post(path.c_str(), body, res), c.device_id, res)) {
                String msg = String("Playing ") + (res["station"] | "radio");
                ui::toast(msg.c_str());
            }
            break;
        }
        case Type::JoinGroup: {
            JsonDocument body, res;
            body["host_device_id"] = c.value;
            if (report(api::post(device_path(c.device_id, "multiroom/join").c_str(), body, res), c.device_id, res)) {
                String msg = String("Joined ") + (res["joined"] | "group");
                ui::toast(msg.c_str());
            }
            break;
        }
        case Type::LeaveGroup: {
            JsonDocument res;
            if (report(api::del(device_path(c.device_id, "multiroom/leave").c_str(), res), c.device_id, res))
                ui::toast("Left the group");
            break;
        }
        case Type::Volume: break;  // handled below
    }
}

void loop() {
    Command c;
    // One command per loop() pass keeps MQTT and garnet_web responsive
    if (queue && xQueueReceive(queue, &c, 0) == pdTRUE) run(c);

    unsigned long now = millis();
    if (now - last_volume_ms < kVolumeIntervalMs) return;
    portENTER_CRITICAL(&volume_mux);
    int device = volume_device, value = volume_value;
    volume_value = -1;
    portEXIT_CRITICAL(&volume_mux);
    if (value < 0) return;

    last_volume_ms = now;
    JsonDocument v;
    v.set(value);
    put_value(device, "volume", "volume", v.as<JsonVariantConst>());
}

}  // namespace commands
