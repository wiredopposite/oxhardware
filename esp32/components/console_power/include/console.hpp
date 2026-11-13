#pragma once

// The console's power state, from the FPGA (docs/01-interface.md §5.2 LPC_STATUS): its
// LPC clock runs whenever it is powered, LRESET# says whether it is out of reset. And its
// front-panel buttons, pressed through open-drain GPIOs. Needs the board on standby power
// to see the console off or to turn it on.

#include <cstdint>

namespace Console {

enum class State : uint8_t {
    Unknown = 0,        // the FPGA doesn't answer
    Off = 1,            // no LPC clock
    Reset = 2,          // clock running, LRESET# asserted (booting or resetting)
    Running = 3,
};

enum class Button : uint8_t {
    Power = 0,
    Eject = 1,
};

void Start();
State Get();
const char *Name(State s);

// A short press (200 ms), from any task; false while another press is in progress
bool Press(Button b);

// Turns the console on when it is off: a Bluetooth controller has connected. At most once
// per WakeHoldoffMs, so a console that doesn't come on isn't pressed again and again.
void WakeIfOff();

// Called (on the console task) once the console has been off for 5 s after it ran: shut
// down, not an SMC power cycle, which comes back on sooner
using Handler = void (*)();
void OnShutdown(Handler h);

void RegisterCommands();

}
