#include "settings.h"

#include <Preferences.h>
#include <garnet_settings.h>

#include "model.h"

namespace settings {

static const char *kServerGroup = "remoment";
static const char *kRegNamespace = "rm_reg";  // raw Preferences, hidden from the web UI

static volatile bool rediscover_requested = false;

// garnet_web runs button actions and onChange hooks on the loop task (via gwLoop)
static void request_rediscover() { rediscover_requested = true; }
static void on_server_changed(const GsField &) { rediscover_requested = true; }

static void forget_registration() {
    clear_registration();
    rediscover_requested = true;
}

static bool valid_url_or_empty(const String &v) { return v.isEmpty() || v.startsWith("http://"); }

static const GsField kServerFields[] = {
    gsWithHelp(gsWithPlaceholder(gsWithValidate(gsText("url", "Server URL", "", 95, on_server_changed),
                                                valid_url_or_empty, "must start with http://"),
                                 "http://192.168.1.10"),
               "Leave empty to find the server automatically (mDNS)"),
    gsButton("find", "Find server again", request_rediscover),
    gsButton("forget", "Forget registration", forget_registration,
             "The server will see this screen as a new client that needs approval again."),
};

// ---- Status (read-only, replaces the old /status JSON endpoint) -------------
// Info getters run on the HTTP server task: read-only access to the model only.

static String phase_str() {
    switch (model.phase) {
        case Phase::Network:     return "Waiting for network";
        case Phase::Discovering: return "Looking for server";
        case Phase::Registering: return "Waiting for approval";
        case Phase::Running:     return "Running";
    }
    return "";
}

static String server_str() { return model.server_url[0] ? String(model.server_url) : String("-"); }
static String mqtt_str() {
    if (!model.mqtt_host[0]) return "-";
    return String(model.mqtt_host) + ":" + model.mqtt_port + (model.mqtt_connected ? " (connected)" : " (disconnected)");
}
static String pairing_str() { return model.pairing_code[0] ? String(model.pairing_code) : String("-"); }
static String device_str() {
    const Device *d = model.device();
    return d ? String(d->name) + " (#" + d->id + ")" : String("-");
}
static String devices_str() { return String(model.device_count); }
static String track_str() {
    const Device *d = model.device();
    if (!d || !d->active()) return "Not playing";
    return String(d->artist) + " - " + d->track + " (" + d->progress + "%)";
}
static String battery_str() {
    return model.battery_voltage > 0.0f ? String(model.battery_voltage, 2) + " V" : String("-");
}
static String sd_str() { return model.sd_available ? "Mounted" : "Not found"; }

static const GsField kStatusFields[] = {
    gsInfo("phase", "State", phase_str),
    gsInfo("server", "Server", server_str),
    gsInfo("mqtt", "MQTT", mqtt_str),
    gsInfo("pairing", "Pairing code", pairing_str),
    gsInfo("device", "Device", device_str),
    gsInfo("devices", "Assigned devices", devices_str),
    gsInfo("track", "Now playing", track_str),
    gsInfo("battery", "Battery", battery_str),
    gsInfo("sd", "SD card", sd_str),
};

static const GsGroup kServerGroupDef = gsGroup(kServerGroup, "Server", "server", kServerFields, "ReMoment");
static const GsGroup kStatusGroupDef = gsGroup("rm_status", "Status", "info", kStatusFields, "ReMoment");

void register_groups() {
    gsRegister(kStatusGroupDef);
    gsRegister(kServerGroupDef);
}

String server_url() { return gsGetString(kServerGroup, "url"); }

// ---- Registration storage ----------------------------------------------------

static String reg_get(const char *key) {
    if (!gsNamespaceExists(kRegNamespace)) return "";
    Preferences prefs;
    prefs.begin(kRegNamespace, true);
    String v = prefs.getString(key, "");
    prefs.end();
    return v;
}

String registration_token() { return reg_get("reg"); }
String api_token() { return reg_get("api"); }

void set_registration(const String &registration_token, const String &api_token) {
    Preferences prefs;
    prefs.begin(kRegNamespace, false);
    prefs.putString("reg", registration_token);
    prefs.putString("api", api_token);
    prefs.end();
}

void clear_registration() { set_registration("", ""); }

int last_device_id() {
    if (!gsNamespaceExists(kRegNamespace)) return -1;
    Preferences prefs;
    prefs.begin(kRegNamespace, true);
    int id = prefs.getInt("device", -1);
    prefs.end();
    return id;
}

void set_last_device_id(int id) {
    Preferences prefs;
    prefs.begin(kRegNamespace, false);
    prefs.putInt("device", id);
    prefs.end();
}

bool take_rediscover_request() {
    bool requested = rediscover_requested;
    rediscover_requested = false;
    return requested;
}

}  // namespace settings
