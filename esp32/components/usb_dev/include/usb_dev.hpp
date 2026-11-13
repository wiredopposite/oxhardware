#pragma once

// The console's controller on the FPGA's USB port (docs/03-esp32.md §8): a 3-port hub
// with the XID gamepad on port 1 (ports 2 and 3 for memory units). One task runs usbd and
// the FPGA driver, woken by the FPGA's USB interrupt.

#include <cstdint>

namespace UsbDev {

// At boot: attaches as soon as the FPGA has USB, and again whenever the FPGA is reloaded
void Start();
void RegisterCommands();

// The gamepad, from any task: its state (xid_in_t, 18 bytes), whether a controller is
// behind it (plugged into hub port 1 or not; kept for the next attach, and present until
// first set), and the console's rumble
void SetPadState(const uint8_t state[18]);
void SetPadPresent(bool present);
using RumbleHandler = void (*)(uint16_t left, uint16_t right);
void OnRumble(RumbleHandler handler);

}
