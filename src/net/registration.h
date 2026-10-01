// Client registration with the ReMoment server (see docs/api/client-devices.md in ReMomentServer):
// register -> admin approves in the server UI -> api_token -> assigned devices + heartbeat.
#pragma once

namespace registration {

enum class Result {
    Approved,  // api_token valid, model.devices filled
    Pending,   // waiting for admin approval, model.pairing_code set
    Failed,    // server unreachable or unexpected answer; retry later
};

// Runs one step of the flow. Cheap to call repeatedly; it rate-limits itself.
Result step();

// Re-fetches the assigned device list (keeps the current device when still assigned).
bool refresh_devices();

// PUT heartbeat when due; call from loop().
void heartbeat_loop();

// Drops all in-memory state (after "forget registration" or a server change).
void reset();

}  // namespace registration
