// ES8311 codec + speaker on the ES3C28P, used for short UI feedback sounds.
#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"

namespace board::es3c28p_audio {

// Codec over I2C (shared with touch) and I2S output. Returns false when the codec doesn't answer.
bool init(i2c_master_bus_handle_t bus);

// Queues a short tick; returns immediately (played on the audio task)
void click();

// 0-100; 0 = silent
void set_volume(uint8_t percent);

}  // namespace board::es3c28p_audio
