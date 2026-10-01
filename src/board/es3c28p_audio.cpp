// ES8311 playback bring-up, following Espressif's esp_codec_dev es8311 driver (es8311_open,
// es8311_config_sample for MCLK 4.096 MHz / 16 kHz, es8311_start in DAC mode) as register
// writes, so the whole codec component isn't needed under Arduino. Speaker playback on this
// board was never tried in MeetMicRec (mic only) — first use here.
#if defined(BOARD_ES3C28P)

#include "es3c28p_audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

#include "es3c28p_pins.h"

namespace board::es3c28p_audio {

static const uint8_t  kCodecAddr  = 0x18;
static const uint32_t kSampleRate = 16000;
static const int      kClickMs    = 8;

static i2c_master_dev_handle_t codec;
static I2SClass i2s;
static QueueHandle_t queue;
static int16_t click_samples[kSampleRate * kClickMs / 1000];
static volatile uint8_t volume = 60;

static bool write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(codec, buf, sizeof(buf), 50) == ESP_OK;
}

static bool codec_init() {
    bool ok = true;
    // es8311_open
    ok &= write_reg(0x44, 0x08);  // twice, per the vendor driver (I2C noise immunity)
    ok &= write_reg(0x44, 0x08);
    ok &= write_reg(0x01, 0x30);
    ok &= write_reg(0x02, 0x00);
    ok &= write_reg(0x03, 0x10);
    ok &= write_reg(0x16, 0x24);
    ok &= write_reg(0x04, 0x10);
    ok &= write_reg(0x05, 0x00);
    ok &= write_reg(0x0B, 0x00);
    ok &= write_reg(0x0C, 0x00);
    ok &= write_reg(0x10, 0x1F);
    ok &= write_reg(0x11, 0x7F);
    ok &= write_reg(0x00, 0x80);  // power on, slave mode (the ESP32 drives I2S clocks)
    ok &= write_reg(0x01, 0x3F);  // clocks on, MCLK from the pin, not inverted
    ok &= write_reg(0x06, 0x03);  // BCLK not inverted, divider 4 (also es8311_config_sample)
    ok &= write_reg(0x13, 0x10);
    ok &= write_reg(0x1B, 0x0A);
    ok &= write_reg(0x1C, 0x6A);
    // es8311_config_sample: MCLK 4.096 MHz (256 x fs), 16 kHz
    ok &= write_reg(0x02, 0x00);  // pre_div 1, pre_multi x1
    ok &= write_reg(0x05, 0x00);  // adc/dac div 1
    ok &= write_reg(0x03, 0x10);  // single speed, adc osr 0x10
    ok &= write_reg(0x04, 0x20);  // dac osr 0x20
    ok &= write_reg(0x07, 0x00);  // lrck div high
    ok &= write_reg(0x08, 0xFF);  // lrck div low (256)
    // 16-bit I2S on the serial ports; DAC unmuted (bit 6 clear), ADC muted
    ok &= write_reg(0x09, 0x0C);
    ok &= write_reg(0x0A, 0x4C);
    // es8311_start, DAC mode
    ok &= write_reg(0x17, 0xBF);
    ok &= write_reg(0x0E, 0x02);
    ok &= write_reg(0x12, 0x00);  // DAC on
    ok &= write_reg(0x14, 0x1A);
    ok &= write_reg(0x0D, 0x01);  // analog power up
    ok &= write_reg(0x15, 0x40);
    ok &= write_reg(0x37, 0x08);
    ok &= write_reg(0x45, 0x00);
    ok &= write_reg(0x31, 0x00);  // not muted
    ok &= write_reg(0x32, 0xBF);  // DAC volume 0 dB; the click itself carries the level
    return ok;
}

// A soft tick: a short 1.6 kHz tone with a fast exponential decay
static void make_click() {
    const int n = sizeof(click_samples) / sizeof(click_samples[0]);
    for (int i = 0; i < n; i++) {
        float t        = (float)i / kSampleRate;
        float envelope = expf(-t * 900.0f);
        click_samples[i] = (int16_t)(sinf(2 * PI * 1600.0f * t) * envelope * 12000.0f);
    }
}

static void audio_task(void *) {
    static int16_t buf[sizeof(click_samples) / sizeof(click_samples[0])];
    uint8_t dummy;
    for (;;) {
        if (xQueueReceive(queue, &dummy, portMAX_DELAY) != pdTRUE) continue;
        uint8_t v = volume;
        if (v == 0) continue;
        for (size_t i = 0; i < sizeof(buf) / sizeof(buf[0]); i++) buf[i] = (int32_t)click_samples[i] * v / 100;
        i2s.write((const uint8_t *)buf, sizeof(buf));
    }
}

bool init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = kCodecAddr;
    dev_cfg.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &codec) != ESP_OK) return false;

    // I2S first: the codec needs MCLK running while it's configured
    i2s.setPins(ES3C28P_I2S_SCK, ES3C28P_I2S_LRC, ES3C28P_I2S_DO, -1, ES3C28P_I2S_MCK);
    if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("[audio] I2S start failed");
        return false;
    }
    if (!codec_init()) {
        Serial.println("[audio] ES8311 not answering");
        return false;
    }

    pinMode(ES3C28P_AUDIO_EN, OUTPUT);
    digitalWrite(ES3C28P_AUDIO_EN, LOW);  // speaker amplifier on (active low)

    make_click();
    queue = xQueueCreate(2, 1);
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, nullptr, 3, nullptr, 0);
    Serial.println("[audio] ES8311 ready");
    return true;
}

void click() {
    uint8_t one = 1;
    if (queue) xQueueSend(queue, &one, 0);  // drop when one is already pending
}

void set_volume(uint8_t percent) { volume = min<uint8_t>(percent, 100); }

}  // namespace board::es3c28p_audio

#endif
