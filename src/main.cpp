/*
 * ReMomentClient — touch remote for the ReMoment server.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "config.h"
#include "io_extension.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "user_sd.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#define PLAYER_TOPIC(sub) "remoment/player/" STR(PLAYER_ID) "/" sub

static const char *TOPIC_DATA     = PLAYER_TOPIC("data");
static const char *TOPIC_PROGRESS = PLAYER_TOPIC("progress");

static const unsigned long NOT_PLAYING_TIMEOUT_MS     = 15000;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000;
static const unsigned long BATTERY_SAMPLE_INTERVAL_MS = 5000;

static const float BATTERY_V_MIN = 3.0f;
static const float BATTERY_V_MAX = 4.2f;

static WiFiClient   wifi_client;
static PubSubClient mqtt_client(wifi_client);
static WebServer    debug_server(80);

// Fixed buffers to avoid heap fragmentation
static char track_artist[64];
static char track_name[64];
static int  track_progress = 0;  // 0-100

static bool          is_playing   = false;
static unsigned long last_data_ms = 0;  // millis() of last received MQTT data
static unsigned long last_mqtt_attempt_ms = 0;

static const char *active_screen = "startup";
static bool        sd_available  = false;

// Battery is sampled in loop() so the debug endpoint never blocks on I2C
static float         battery_voltage = 0.0f;
static unsigned long last_battery_ms = 0;

// ---------------------------------------------------------------------------
// UI — LVGL runs in its own task, so every widget access must hold the port mutex
// ---------------------------------------------------------------------------

static void ui_show_track() {
    if (!lvgl_port_lock(-1)) return;
    active_screen = "now_playing";
    lv_label_set_text(ui_track_name, track_name);
    lv_label_set_text(ui_Artist, track_artist);
    lvgl_port_unlock();
}

static void ui_show_progress() {
    if (!lvgl_port_lock(-1)) return;
    lv_slider_set_value(uic_playing_progress, track_progress, LV_ANIM_ON);
    lvgl_port_unlock();
}

static void ui_set_playing(bool playing) {
    if (!lvgl_port_lock(-1)) return;
    if (playing) {
        lv_obj_set_style_img_opa(ui_album_art, LV_OPA_COVER, 0);
    } else {
        lv_label_set_text(ui_track_name, "—");  // em dash
        lv_label_set_text(ui_Artist, "");
        lv_slider_set_value(uic_playing_progress, 0, LV_ANIM_OFF);
        lv_obj_set_style_img_opa(ui_album_art, LV_OPA_50, 0);
    }
    lvgl_port_unlock();
}

static void mark_data_received() {
    last_data_ms = millis();
    if (!is_playing) {
        is_playing = true;
        ui_set_playing(true);
    }
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

static void sample_battery() {
    // 10-bit ADC behind a 1:3 divider, 3.3 V reference
    float v = IO_EXTENSION_Adc_Input() * 3 * 3.3f / 1023.0f;
    battery_voltage = min(v, BATTERY_V_MAX);
}

static int battery_percent() {
    int pct = (int)((battery_voltage - BATTERY_V_MIN) / (BATTERY_V_MAX - BATTERY_V_MIN) * 100.0f);
    return constrain(pct, 0, 100);
}

// ---------------------------------------------------------------------------
// Debug HTTP endpoint: GET /status
// ---------------------------------------------------------------------------

static const char *reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_EXT:       return "external";
        default:                return "unknown";
    }
}

static void handle_debug_status() {
    JsonDocument doc;

    doc["player"]   = PLAYER_ID;
    doc["screen"]   = active_screen;
    doc["playing"]  = is_playing;
    doc["track"]    = track_name;
    doc["artist"]   = track_artist;
    doc["progress"] = track_progress;
    doc["uptime_s"] = millis() / 1000;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"]    = WiFi.SSID();
    wifi["ip"]      = WiFi.localIP().toString();
    wifi["mac"]     = WiFi.macAddress();
    wifi["rssi"]    = WiFi.RSSI();
    wifi["channel"] = WiFi.channel();

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["connected"] = mqtt_client.connected();
    if (last_data_ms > 0) {
        mqtt["seconds_since_last_msg"] = (millis() - last_data_ms) / 1000;
    } else {
        mqtt["seconds_since_last_msg"] = nullptr;
    }

    JsonObject cpu = doc["cpu"].to<JsonObject>();
    cpu["freq_mhz"]     = ESP.getCpuFreqMHz();
    cpu["temp_c"]       = temperatureRead();
    cpu["reset_reason"] = reset_reason_str();
    cpu["rtos_tasks"]   = uxTaskGetNumberOfTasks();

    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["heap_free"]     = ESP.getFreeHeap();
    mem["heap_total"]    = ESP.getHeapSize();
    mem["heap_min_free"] = ESP.getMinFreeHeap();
    mem["psram_free"]    = ESP.getFreePsram();
    mem["psram_total"]   = ESP.getPsramSize();
    mem["sketch_size"]   = ESP.getSketchSize();
    mem["sketch_free"]   = ESP.getFreeSketchSpace();

    JsonObject bat = doc["battery"].to<JsonObject>();
    if (battery_voltage > 0.0f) {
        bat["voltage"] = serialized(String(battery_voltage, 2));
        bat["percent"] = battery_percent();
    } else {
        bat["voltage"] = nullptr;
        bat["percent"] = nullptr;
    }

    JsonObject sd = doc["sd"].to<JsonObject>();
    sd["installed"] = sd_available;
    if (sd_available) {
        uint64_t total_kb = 0, free_kb = 0;
        if (read_sd_capacity(&total_kb, &free_kb) == ESP_OK) {
            sd["total_mb"] = (unsigned long)(total_kb / 1024);
            sd["free_mb"]  = (unsigned long)(free_kb / 1024);
        }
    }

    String response;
    serializeJsonPretty(doc, response);
    debug_server.send(200, "application/json", response);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

static void wifi_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // Modem sleeps between beacons, reduces PSRAM bus contention
    WiFi.setTxPower(WIFI_POWER_15dBm);
}

static void mqtt_callback(char *topic, byte *payload, unsigned int length) {
    if (strcmp(topic, TOPIC_DATA) == 0) {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length)) return;

        strlcpy(track_artist, doc["artist"] | "", sizeof(track_artist));
        strlcpy(track_name, doc["track"] | "", sizeof(track_name));
        ui_show_track();
        mark_data_received();
    } else if (strcmp(topic, TOPIC_PROGRESS) == 0) {
        // Plain numeric payload, 0-100
        char value[8];
        if (length >= sizeof(value)) return;
        memcpy(value, payload, length);
        value[length] = '\0';

        track_progress = constrain(atoi(value), 0, 100);
        ui_show_progress();
        mark_data_received();
    }
}

static void mqtt_connect() {
    if (mqtt_client.connected()) return;

    unsigned long now = millis();
    if (last_mqtt_attempt_ms != 0 && now - last_mqtt_attempt_ms < MQTT_RECONNECT_INTERVAL_MS) return;
    last_mqtt_attempt_ms = now;

    if (mqtt_client.connect(MQTT_CLIENT_ID)) {
        Serial.println("MQTT connected");
        mqtt_client.subscribe(TOPIC_DATA);
        mqtt_client.subscribe(TOPIC_PROGRESS);
    } else {
        Serial.printf("MQTT connect failed, rc=%d\n", mqtt_client.state());
    }
}

// ---------------------------------------------------------------------------

void setup() {
    // Must run first: also brings up the shared I2C bus and IO expander
    esp_lcd_touch_handle_t tp_handle = touch_gt911_init();

    Serial.begin(115200);

    esp_lcd_panel_handle_t panel_handle = waveshare_esp32_s3_rgb_lcd_init();
    waveshare_rgb_lcd_bl_on();

    ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));
    if (lvgl_port_lock(-1)) {
        ui_init();
        lvgl_port_unlock();
    }

    wifi_connect();

    mqtt_client.setServer(MQTT_HOST, MQTT_PORT);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_client.setSocketTimeout(3);
    mqtt_connect();

    sd_available = (sd_mmc_init() == ESP_OK);
    Serial.printf("SD card: %s\n", sd_available ? "found" : "not found");

    debug_server.on("/status", handle_debug_status);
    debug_server.begin();
    Serial.printf("Debug server: http://%s/status\n", WiFi.localIP().toString().c_str());
}

void loop() {
    mqtt_connect();
    mqtt_client.loop();

    if (is_playing && millis() - last_data_ms > NOT_PLAYING_TIMEOUT_MS) {
        is_playing = false;
        ui_set_playing(false);
    }

    debug_server.handleClient();

    if (millis() - last_battery_ms > BATTERY_SAMPLE_INTERVAL_MS) {
        last_battery_ms = millis();
        sample_battery();
    }
}
