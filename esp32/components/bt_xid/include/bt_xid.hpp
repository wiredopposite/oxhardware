#pragma once

// Bluetooth controllers as the console's gamepad: BlueRetro (the blueretro component)
// with an XID output (xid_map.c). Output 0 feeds the Xbox USB profile's pad: its state,
// whether it is plugged into the controller hub, and the console's rumble back.

namespace BtXid {

void Start();
void RegisterCommands();

}
