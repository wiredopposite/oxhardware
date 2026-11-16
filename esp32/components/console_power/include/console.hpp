#pragma once

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

bool Press(Button b);

void WakeIfOff();

// Called once the console has been off for 5s after running, which tells a shutdown
// from an SMC power cycle
using Handler = void (*)();
void OnShutdown(Handler h);

void RegisterCommands();

}
