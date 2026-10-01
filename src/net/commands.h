// User actions from the UI (LVGL task) to the server. post() never blocks; loop()
// runs on the Arduino loop task, does the HTTP calls and reports back through ui::.
#pragma once

#include <stdint.h>

namespace commands {

enum class Type : uint8_t {
    Play,
    Pause,
    Next,
    Previous,
    Volume,          // value 0-100 (coalesced: only the latest is sent)
    Mute,            // value 0/1
    Shuffle,         // value 0/1
    Repeat,          // value: Repeat enum
    Like,            // value 0/1
    Power,           // value 0 = standby, 1 = on
    SelectDevice,    // value = device id
    LoadDetails,     // fetch sources / radio / multiroom for the details screen
    ActivateSource,  // text = source_id
    PlayRadio,       // value = station id
    JoinGroup,       // value = host device id
    LeaveGroup,
};

struct Command {
    Type type;
    int  device_id;
    int  value;
    char text[48];
};

void begin();

// Safe from any task
void post(Type type, int device_id, int value = 0, const char *text = nullptr);

void loop();

}  // namespace commands
