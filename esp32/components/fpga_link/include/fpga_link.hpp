#pragma once

// ESP32 <-> FPGA link (docs/01-interface.md §3): SPI mode 0 on VSPI through the GPIO
// matrix. Every transaction returns the FPGA's status byte (STS).

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
// Link SCK frequency (default 10 MHz; 20 MHz maximum, GPIO-matrix input delay).
void SetClock(int hz);

// Reads len (<= MaxXfer) bytes from space:addr. noinc reads the same address repeatedly.
bool Read(Space space, uint32_t addr, uint8_t *buf, size_t len, uint8_t *sts = nullptr,
          bool noinc = false);
bool Write(Space space, uint32_t addr, const uint8_t *data, size_t len,
           uint8_t *sts = nullptr, bool noinc = false);

// Wiring check: bit-bangs the link pins. Returns MISO levels (bit0 CS high + pull-up,
// bit1 CS high + pull-down, bit2 CS low); rx = STS + the 4 ID bytes, rx[5] unused.
uint8_t Probe(uint8_t rx[6]);

void RegisterCommands();

}
