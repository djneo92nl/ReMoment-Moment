#include "player.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <garnet_web.h>

#include "model.h"
#include "ui/ui.h"

namespace player {

static const unsigned long kNotPlayingTimeoutMs     = 15000;
static const unsigned long kMqttReconnectIntervalMs = 10000;

static WiFiClient   wifi_client;
static PubSubClient mqtt(wifi_client);

static unsigned long last_attempt_ms = 0;
static char subscribed_topic[sizeof(Device::mqtt_topic)] = "";
static char topic_data[64];
static char topic_progress[64];

static void mark_data_received() {
    model.last_data_ms = millis();
    if (!model.is_playing) {
        model.is_playing = true;
        ui::now_playing_set_playing(true);
    }
}

static void on_message(char *topic, byte *payload, unsigned int length) {
    if (strcmp(topic, topic_data) == 0) {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length)) return;
        strlcpy(model.track_artist, doc["artist"] | "", sizeof(model.track_artist));
        strlcpy(model.track_name, doc["track"] | "", sizeof(model.track_name));
        ui::now_playing_set_track(model.track_artist, model.track_name);
        mark_data_received();
    } else if (strcmp(topic, topic_progress) == 0) {
        // Plain numeric payload, 0-100
        char value[8];
        if (length >= sizeof(value)) return;
        memcpy(value, payload, length);
        value[length] = '\0';
        model.track_progress = constrain(atoi(value), 0, 100);
        ui::now_playing_set_progress(model.track_progress);
        mark_data_received();
    }
}

static void unsubscribe() {
    if (subscribed_topic[0] && mqtt.connected()) {
        mqtt.unsubscribe(topic_data);
        mqtt.unsubscribe(topic_progress);
    }
    subscribed_topic[0] = '\0';
}

static void subscribe() {
    const Device *d = model.device();
    if (!d || !mqtt.connected()) return;
    snprintf(topic_data, sizeof(topic_data), "%s/data", d->mqtt_topic);
    snprintf(topic_progress, sizeof(topic_progress), "%s/progress", d->mqtt_topic);
    mqtt.subscribe(topic_data);
    mqtt.subscribe(topic_progress);
    strlcpy(subscribed_topic, d->mqtt_topic, sizeof(subscribed_topic));
}

static void connect() {
    if (mqtt.connected() || !model.mqtt_host[0]) return;
    unsigned long now = millis();
    if (last_attempt_ms != 0 && now - last_attempt_ms < kMqttReconnectIntervalMs) return;
    last_attempt_ms = now;

    mqtt.setServer(model.mqtt_host, model.mqtt_port);
    String client_id = "remoment-client-" + gwChipId();
    if (mqtt.connect(client_id.c_str())) {
        Serial.println("[mqtt] connected");
        subscribed_topic[0] = '\0';
        subscribe();
    } else {
        Serial.printf("[mqtt] connect to %s:%u failed, rc=%d\n", model.mqtt_host, model.mqtt_port, mqtt.state());
    }
}

void begin() {
    mqtt.setCallback(on_message);
    mqtt.setSocketTimeout(3);
    mqtt.setBufferSize(1024);  // /data carries artwork URLs and colors
}

void device_changed() {
    unsubscribe();
    model.is_playing      = false;
    model.track_artist[0] = '\0';
    model.track_name[0]   = '\0';
    model.track_progress  = 0;
    const Device *d = model.device();
    ui::now_playing_set_device(d ? d->name : "");
    ui::now_playing_set_playing(false);
    subscribe();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        model.mqtt_connected = false;
        return;
    }
    connect();
    mqtt.loop();
    model.mqtt_connected = mqtt.connected();

    if (model.is_playing && millis() - model.last_data_ms > kNotPlayingTimeoutMs) {
        model.is_playing = false;
        ui::now_playing_set_playing(false);
    }
}

}  // namespace player
