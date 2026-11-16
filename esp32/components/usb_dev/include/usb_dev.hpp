#pragma once

#include <cstdint>

namespace UsbDev {
void Start();
void RegisterCommands();

// From any task. state is an xid_in_t.
void SetPadState(const uint8_t state[18]);
void SetPadPresent(bool present);
using RumbleHandler = void (*)(uint16_t left, uint16_t right);
void OnRumble(RumbleHandler handler);

}
