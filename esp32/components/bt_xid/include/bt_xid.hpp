#pragma once

// Bluetooth controllers as the console's gamepad: BlueRetro (the blueretro component)
// with an XID output (xid_map.c). The first controller to connect feeds the Xbox USB
// profile's pad (its state, whether the pad is plugged into the controller hub, and the
// console's rumble back); when it goes, another connected controller takes over.

namespace BtXid {

void Start();
void RegisterCommands();

}
