#pragma once

// The console's SMBus. A slave at 0x60, and a master only for each transfer; the MCPX is
// the other master, so transfers that lose arbitration are retried.

#include <cstddef>
#include <cstdint>

namespace Smbus {
namespace Addr {
inline constexpr uint8_t Smc = 0x10;
inline constexpr uint8_t Temp = 0x4C;
inline constexpr uint8_t Eeprom = 0x54;
}

// SMC register 0x02 values
enum class Power : uint8_t {
    Reset = 0x01,
    Cycle = 0x40,
    Off = 0x80,
};

void Init();

bool ReadByte(uint8_t addr, uint8_t reg, uint8_t *val);
bool ReadWord(uint8_t addr, uint8_t reg, uint16_t *val);
bool WriteByte(uint8_t addr, uint8_t reg, uint8_t val);
bool WriteWord(uint8_t addr, uint8_t reg, uint16_t val);

bool SmcPower(Power action);

void RegisterCommands();

}
