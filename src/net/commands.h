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
    LoadLibrary,     // value = ui::LibView, text = cursor ("" = first page)
    LoadArtist,      // value = artist id
    LoadAlbum,       // value = album id
    PlayAlbum,       // value = album id, value2 = start track id (0 = from the start)
    PlayArtist,      // value = artist id (shuffled)
    FavoriteAlbum,   // value = album id, value2 = 0/1
    FavoriteArtist,  // value = artist id, value2 = 0/1
    LoadSettingsInfo,
    LoadPlaylist,    // value = playlist id
    PlayPlaylist,    // value = playlist id, value2 = start track id (0 = from the start)
    LoadSource,      // active source of the device, for the volume screen
    LoadQueue,       // upcoming tracks of the device, for the queue screen
    LoadSearch,      // text = query (at least 2 characters)
    Seek,            // value = position in seconds
    QueueJump,       // value = places ahead in the queue (1 = next track)
};

struct Command {
    Type type;
    int  device_id;
    int  value;
    int  value2;
    char text[48];
};

void begin();

// Safe from any task
void post(Type type, int device_id, int value = 0, const char *text = nullptr, int value2 = 0);

void loop();

}  // namespace commands
