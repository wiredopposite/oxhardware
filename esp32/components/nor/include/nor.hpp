#pragma once

// The FPGA's flash, reached through its NOR engine over the link (docs/01-interface.md
// §5.5) while the FPGA keeps serving the BIOS. While the FPGA is held in reset the same
// calls go over the config bus instead.
//
// The engine refuses erase and program below the ROM catalogue (the bitstream).

#include <cstddef>
#include <cstdint>

namespace Nor {

void Init();

// The engine has woken the flash and read its JEDEC ID.
bool Ready();
bool ReadId(uint8_t id[3]);
bool Read(uint32_t addr, uint8_t *buf, size_t len);
// Erases [addr, addr + len): addr and len multiples of 4 KB; 64 KB blocks where aligned.
bool Erase(uint32_t addr, uint32_t len);
// Programs data (any alignment; split into pages). The range must be erased.
bool Write(uint32_t addr, const uint8_t *data, size_t len);
bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc);

void RegisterCommands();

}
