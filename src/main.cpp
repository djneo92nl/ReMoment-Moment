/*
* Ported LVGL 8.4 and display the official demo interface.
*/
#include <Arduino.h>

#include "lvgl_port.h"       // LVGL porting functions for integration
#include <demos/lv_demos.h>        // LVGL demo headers
#include "ui/ui.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include <ArduinoJson.h>

// WIFI
const char* ssid     = "IDiot";
const char* password = "WIFI01010";

// MQTT


WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* mqtt_server = "192.168.1.247";  // your broker
const int   mqtt_port   = 1883;

String incomingTrackName;
volatile bool trackNameUpdated = false;

// Fixed buffers to avoid heap fragmentation
char track_artist[64];
char track_name[64];

// Update labels
void update_player_data(void *param) {
    lv_label_set_text(ui_track_name, track_name);
    lv_label_set_text(ui_Artist, track_artist);
}

// Update progress slider
int new_progress = 0; // 0-100
void update_player_progress(void *param) {
    lv_slider_set_value(uic_playing_progress, new_progress, LV_ANIM_ON);
}

void wifi_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
}
void update_track_label(void * param) {
    lv_label_set_text(ui_track_name, incomingTrackName.c_str());
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    // Convert payload to string safely
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';

    // Check topic
    if (strstr(topic, "/data")) {
        // JSON payload
        StaticJsonDocument<200> doc;  // adjust size if more fields
        DeserializationError err = deserializeJson(doc, message);
        if (!err) {
            const char* artist = doc["artist"] | "";
            const char* track  = doc["track"]  | "";

            strncpy(track_artist, artist, sizeof(track_artist)-1);
            track_artist[sizeof(track_artist)-1] = '\0';

            strncpy(track_name, track, sizeof(track_name)-1);
            track_name[sizeof(track_name)-1] = '\0';

            lv_async_call(update_player_data, NULL);
        }
    }
    else if (strstr(topic, "/progress")) {
        // Numeric payload, 0-100
        int val = atoi(message);
        if (val < 0) val = 0;
        if (val > 100) val = 100;

        new_progress = val;
        lv_async_call(update_player_progress, NULL);
    }
}

unsigned long lastReconnectAttempt = 0;

void mqtt_connect() {
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            mqttClient.connect("RemomentClient");
                Serial.println("Connected mqtt");
                mqttClient.subscribe("remoment/player/4/data");
    mqttClient.subscribe("remoment/player/4/progress");
        }
    } else {
        mqttClient.loop();
    }
}



void setup() {
    static esp_lcd_panel_handle_t panel_handle = NULL; // Declare a handle for the LCD panel
    static esp_lcd_touch_handle_t tp_handle = NULL;    // Declare a handle for the touch panel

    // Initialize the GT911 touch screen controller
    tp_handle = touch_gt911_init();  

    Serial.begin(115200);
    
    // Initialize the Waveshare ESP32-S3 RGB LCD hardware
    panel_handle = waveshare_esp32_s3_rgb_lcd_init(); 

    // Turn on the LCD backlight
    wavesahre_rgb_lcd_bl_on();   

    // Initialize LVGL with the panel and touch handles
    ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));

    ESP_LOGI(TAG, "Display LVGL demos");


    // Lock the mutex because LVGL APIs are not thread-safe
    if (lvgl_port_lock(-1)) {
        ui_init();

        // Release the mutex after the demo execution
        lvgl_port_unlock();
    }

    
    // We start initing wifi 
    wifi_connect();
esp_wifi_set_ps(WIFI_PS_NONE);       // already done
WiFi.setTxPower(WIFI_POWER_15dBm);   // slightly reduce radio burst

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqtt_callback);
    mqtt_connect();
}

void loop() {
    if (!mqttClient.connected()) {
        mqtt_connect();
    }
    mqttClient.loop();


}
