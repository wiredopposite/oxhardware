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

}
