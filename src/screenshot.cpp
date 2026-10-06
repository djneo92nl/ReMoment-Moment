#include "screenshot.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board/board.h"
#include "model.h"
#include "ui/nav.h"

namespace screenshot {

static constexpr uint8_t  kTrigger  = 0xA5;
static constexpr uint8_t  kNavigate = 0xA6;  // followed by a screen letter, see navigate()
static constexpr uint32_t kDumpBaud = 921600;
static constexpr uint32_t kBaud     = 115200;

static void dump() {
    const board::Info &info = board::info();
    const uint32_t     size = (uint32_t)info.width * info.height * 2;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[screenshot] out of memory");
        return;
    }

    lv_img_dsc_t dsc;
    lv_res_t     res = LV_RES_INV;
    {
        LvglLock lock;
        if (lock) res = lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &dsc, buf, size);
    }
    if (res != LV_RES_OK) {
        Serial.println("[screenshot] snapshot failed");
        heap_caps_free(buf);
        return;
    }

    Serial.printf("RMSHOT %u %u %d %u\n", dsc.header.w, dsc.header.h, LV_COLOR_16_SWAP, (unsigned)kDumpBaud);
    Serial.flush();
    Serial.updateBaudRate(kDumpBaud);
    delay(300);  // the host switches its port speed in this window
    const uint32_t bytes = (uint32_t)dsc.header.w * dsc.header.h * 2;
    for (uint32_t off = 0; off < bytes; off += 1024) Serial.write(buf + off, min<uint32_t>(1024, bytes - off));
    Serial.flush();
    Serial.updateBaudRate(kBaud);
    heap_caps_free(buf);
}

// Also behind the buttons in the web UI. Replies "RMNAV <letter> ok|unknown" on serial for tools/screenshots.py.
bool navigate(char screen) {
    bool ok = true;
    {
        LvglLock lock;
        if (!lock) return false;
        switch (screen) {
            case 'n': ui::nav::to_now_playing(); break;
            case 'v': ui::nav::to_volume(); break;
            case 'q': ui::nav::to_queue(); break;
            case 'd': ui::nav::to_devices(); break;
            case 'e':
                if (const Device *device = model.device()) ui::nav::to_details(device->id);
                else ok = false;
                break;
            case 'l': ui::nav::to_library(); break;
            case 's': ui::nav::to_settings(); break;
            default:  ok = false;
        }
    }
    Serial.printf("RMNAV %c %s\n", screen, ok ? "ok" : "unknown");
    return ok;
}

void poll() {
    while (Serial.available()) {
        const uint8_t byte = Serial.read();
        if (byte == kTrigger) dump();
        else if (byte == kNavigate) {
            const unsigned long start = millis();
            while (!Serial.available() && millis() - start < 100) delay(1);
            if (Serial.available()) navigate((char)Serial.read());
        }
    }
}

}  // namespace screenshot
