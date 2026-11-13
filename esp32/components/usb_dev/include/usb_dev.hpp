#pragma once

// The USB device functions on the FPGA's USB port (docs/03-esp32.md §8): one task runs
// usbd and the FPGA driver, woken by the FPGA's USB interrupt. Started and stopped from
// the PC (oxctl usb).

#include <cstdint>

namespace UsbDev {

enum class Profile : uint8_t {
    HidTest = 0,        // one HID gamepad, for bring-up on a PC
    HubTest = 1,        // a 4-port hub with the test gamepad on port 1
    Xbox = 2,           // a controller: 3-port hub, XID gamepad on port 1 (2, 3: MU slots)
};

void RegisterCommands();

// The Xbox profile's gamepad, from any task: its state (xid_in_t, 18 bytes), whether a
// controller is behind it (plugged into hub port 1 or not; kept for the next start, and
// present until first set), and the console's rumble
void SetPadState(const uint8_t state[18]);
void SetPadPresent(bool present);
using RumbleHandler = void (*)(uint16_t left, uint16_t right);
void OnRumble(RumbleHandler handler);

}
