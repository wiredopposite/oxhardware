#pragma once

#include <cstddef>
#include <cstdint>

namespace FpgaLink {
enum class Space : uint8_t {
    Regs = 0,
    UsbSlots = 1,
    NorRead = 2,
    Fifos = 3,
};

inline constexpr size_t MaxXfer = 4096;

void Init();
// 10MHz by default, 20MHz at most through the GPIO matrix
void SetClock(int hz);

bool Read(Space space, uint32_t addr, uint8_t *buf, size_t len, uint8_t *sts = nullptr,
          bool noinc = false);
bool Write(Space space, uint32_t addr, const uint8_t *data, size_t len,
           uint8_t *sts = nullptr, bool noinc = false);

// Wiring check by bit-banging. Returns MISO levels (bit0 CS high with a pull-up, bit1 CS
// high with a pull-down, bit2 CS low); rx gets the status byte and the 4 ID bytes.
uint8_t Probe(uint8_t rx[6]);

void RegisterCommands();

}
