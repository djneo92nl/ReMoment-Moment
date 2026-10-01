/*
 * ReMomentClient — touch remote for the ReMoment server.
 *
 * Boot flow: WiFi (garnet_web, setup portal when needed) -> find server (settings or mDNS)
 * -> register and wait for approval -> assigned devices -> MQTT now playing.
 */
#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <garnet_web.h>

#include "app_config.h"
#include "artwork/artwork.h"
#include "board/board.h"
#include "model.h"
#include "net/commands.h"
#include "net/player.h"
#include "net/registration.h"
#include "net/server.h"
#include "app_settings.h"
#include "ui/ui.h"

Model model;

static const unsigned long kDiscoverIntervalMs       = 10000;
static const unsigned long kDeviceRefreshIntervalMs  = 5UL * 60 * 1000;
static const unsigned long kBatterySampleIntervalMs  = 5000;

static unsigned long last_discover_ms       = 0;
static unsigned long last_device_refresh_ms = 0;
static unsigned long last_battery_ms        = 0;

// Only touch the status screen when its text actually changes
static void status(const char *message, const char *detail = nullptr, const char *code = nullptr) {
    static String last;
    String key = String(message) + '\n' + (detail ? detail : "") + '\n' + (code ? code : "");
    if (key == last) return;
    last = key;
    ui::show_status(message, detail, code);
}

static bool network_up() {
    GwNetMode mode = gwNetMode();
    return mode == GwNetMode::WiFi || mode == GwNetMode::Ethernet;
}

static void enter_running() {
    model.phase = Phase::Running;
    last_device_refresh_ms = millis();
    player::devices_changed();
    player::device_changed();
    ui::show_now_playing();
}

static void restart_flow() {
    registration::reset();
    model.server_url[0] = '\0';
    model.mqtt_host[0]  = '\0';
    model.phase         = Phase::Network;
    last_discover_ms    = 0;
    player::devices_changed();
    player::device_changed();
}

static String web_password_hint() { return "Settings password: " + settings::web_password_hint(); }

// Logs network / setup-portal transitions (garnet_web itself is silent at our log level)
static void log_network() {
    static int last_mode = -1, last_portal = -1;
    int mode = (int)gwNetMode(), portal = gwPortalActive();
    if (mode == last_mode && portal == last_portal) return;
    last_mode = mode;
    last_portal = portal;
    static const char *kModes[] = {"offline", "ethernet", "wifi", "portal"};
    Serial.printf("[net] %s, setup AP %s%s%s, IP %s\n", mode < 4 ? kModes[mode] : "?", portal ? "on \"" : "off",
                  portal ? WiFi.softAPSSID().c_str() : "", portal ? "\"" : "", gwNetIP().toString().c_str());
}

static void step_network() {
    if (network_up()) {
        model.phase = Phase::Discovering;
        return;
    }
    if (gwPortalActive()) {
        String detail = "Then open http://" + gwNetIP().toString() + " to choose your network";
        // Someone joined the setup network: they'll need the password next
        if (WiFi.softAPgetStationNum() > 0) detail += "\n" + web_password_hint();
        String message = "Connect to WiFi \"" + WiFi.softAPSSID() + "\"";
        status(message.c_str(), detail.c_str());
    } else {
        status("Connecting to WiFi");
    }
}

static void step_discovering() {
    String url = settings::server_url();
    // Without mDNS the server URL has to be entered in the settings page: say where and how
    String detail = url.isEmpty() ? String("Searching the network") : url;
    detail += "\nSettings: http://" + gwNetIP().toString() + " - " + web_password_hint();
    status("Looking for the ReMoment server", detail.c_str());

    unsigned long now = millis();
    if (last_discover_ms != 0 && now - last_discover_ms < kDiscoverIntervalMs) return;
    last_discover_ms = now;
    if (server::discover()) model.phase = Phase::Registering;
}

static void step_registering() {
    switch (registration::step()) {
        case registration::Result::Approved:
            if (model.device_count > 0) {
                enter_running();
            } else {
                status("No devices assigned", "Assign devices to this screen in the ReMoment admin");
            }
            break;
        case registration::Result::Pending: {
            String detail = "Approve this screen in ReMoment under Settings > Clients (" + String(model.server_url) + ")";
            // Older servers have no pairing code: the chip id is what the admin list shows
            String code = model.pairing_code[0] ? String(model.pairing_code) : gwChipId();
            status("Waiting for approval", detail.c_str(), code.c_str());
            break;
        }
        case registration::Result::Failed:
            status("Registering with the server", model.server_ok ? nullptr : "Server not reachable, retrying");
            break;
    }
}

static void step_running() {
    player::loop();
    commands::loop();
    settings::save_if_settled();

    static unsigned long last_idle_check_ms = 0;
    if (millis() - last_idle_check_ms > 1000) {
        last_idle_check_ms = millis();
        ui::idle_check(model.device());
    }
    registration::heartbeat_loop();

    unsigned long now = millis();
    if (now - last_device_refresh_ms > kDeviceRefreshIntervalMs) {
        last_device_refresh_ms = now;
        int before = model.device() ? model.device()->id : -1;
        if (registration::refresh_devices()) {
            int after = model.device() ? model.device()->id : -1;
            player::devices_changed();
            if (after != before) player::device_changed();
        }
    }
}

static void update_connection_icon() {
    static int last = -1;
    bool wifi   = network_up();
    bool server = model.phase != Phase::Running || model.server_ok;
    bool mqtt   = model.phase != Phase::Running || model.mqtt_connected;
    int state = wifi | server << 1 | mqtt << 2;
    if (state == last) return;
    last = state;
    ui::set_connection(wifi, server, mqtt);
}

void setup() {
    Serial.begin(115200);

    board::init();
    ui::init();

    model.sd_available = board::sd_mount();
    Serial.printf("SD card: %s\n", model.sd_available ? "found" : "not found");

    settings::register_groups();
    if (model.sd_available) {
        gwSetSd(SD_MMC);
        gwAddFs("sd", "SD Card", SD_MMC);
    }

    GwConfig cfg;
    cfg.name            = DEVICE_NAME;
    cfg.defaultPassword = WEB_DEFAULT_PASSWORD;
    cfg.appVersion      = FIRMWARE_VERSION;
    gwBegin(cfg);

    // garnet_web's Clock defaults to UTC; start this household's devices on Amsterdam time
    // (index 2 in its zone list). Changeable in the web settings under Clock.
    if (!gsNamespaceExists("time")) {
        gsSetInt("time", "tz", 2);
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();
    }

    artwork::begin(model.sd_available);
    commands::begin();
    player::begin();
}

void loop() {
    gwLoop();
    log_network();

    if (settings::take_rediscover_request()) restart_flow();

    switch (model.phase) {
        case Phase::Network:     step_network(); break;
        case Phase::Discovering: step_discovering(); break;
        case Phase::Registering: step_registering(); break;
        case Phase::Running:     step_running(); break;
    }
    update_connection_icon();

    if (millis() - last_battery_ms > kBatterySampleIntervalMs) {
        last_battery_ms       = millis();
        model.battery_voltage = board::battery_voltage();
    }
}
