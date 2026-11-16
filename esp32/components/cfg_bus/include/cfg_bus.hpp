#pragma once

#include <cstddef>
#include <cstdint>

namespace CfgBus {
void Init();

bool Cdone();
bool Held();

// Called on the holding task: before CRESET_B goes low (Hold, Probe), after it goes high
// again (Release, Probe). The FPGA's design is gone in between.
using ResetHandler = void (*)();
void OnReset(ResetHandler before, ResetHandler after);

bool Hold();
// Releases the bus and CRESET_B; the FPGA reconfigures from flash. Returns the time to
// CDONE high in microseconds, or -1 if it didn't rise within timeout_ms.
int64_t Release(uint32_t timeout_ms = 1000);

// Wiring check: holds the FPGA, returns the undriven levels of the bus pins (bit0 CS,
// 1 SCK, 2 MOSI, 3 MISO) and reads the JEDEC ID by bit-banging. 0xFF if already held.
uint8_t Probe(uint8_t id[3]);

// Takes the SPI bus with the FPGA running, once its design has let go of the flash pins
// (Nor asks it to over the link); Give() floats the bus again after the flash is idle.
bool Take();
void Give();

// Flash operations; only valid while held or taken.
bool ReadId(uint8_t id[3]);
bool Read(uint32_t addr, uint8_t *buf, size_t len);
// Erases [addr, addr + len): addr and len multiples of 4 KB; 64 KB blocks where aligned.
bool Erase(uint32_t addr, uint32_t len);
// Programs data (any alignment; split into pages). The range must be erased.
bool Write(uint32_t addr, const uint8_t *data, size_t len);
bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc);

void RegisterCommands();

}
