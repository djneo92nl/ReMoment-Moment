#include "server.h"

#include <ESPmDNS.h>

#include "api.h"
#include "app_config.h"
#include "model.h"
#include "settings.h"

namespace server {

static bool find_via_mdns(String &url) {
    int count = MDNS.queryService(REMOMENT_MDNS_SERVICE, REMOMENT_MDNS_PROTO);
    if (count <= 0) return false;

    // First answer wins; there's normally one server on the network
    url = "http://" + MDNS.address(0).toString();
    if (MDNS.port(0) != 80) url += ":" + String(MDNS.port(0));
    Serial.printf("[server] mDNS: %s (%s)\n", url.c_str(), MDNS.hostname(0).c_str());
    return true;
}

static String host_of(const String &url) {
    int start = url.indexOf("://");
    start = start < 0 ? 0 : start + 3;
    int end = url.indexOf(':', start);
    if (end < 0) end = url.indexOf('/', start);
    return end < 0 ? url.substring(start) : url.substring(start, end);
}

bool discover() {
    String url = settings::server_url();
    if (url.isEmpty() && !find_via_mdns(url)) return false;
    while (url.endsWith("/")) url.remove(url.length() - 1);

    JsonDocument info;
    int status = api::get_url((url + "/api/info").c_str(), info);
    if (status <= 0) return false;

    strlcpy(model.server_url, url.c_str(), sizeof(model.server_url));
    model.server_ok = true;

    // Older servers have no /api/info: fall back to a broker on the server host
    const char *mqtt_host = info["mqtt"]["host"] | "";
    strlcpy(model.mqtt_host, *mqtt_host ? mqtt_host : host_of(url).c_str(), sizeof(model.mqtt_host));
    model.mqtt_port = info["mqtt"]["port"] | 1883;

    Serial.printf("[server] %s, MQTT %s:%u\n", model.server_url, model.mqtt_host, model.mqtt_port);
    return true;
}

}  // namespace server
