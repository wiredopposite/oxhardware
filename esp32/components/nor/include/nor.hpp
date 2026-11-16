#pragma once

// The FPGA's flash over the config bus, borrowed from the FPGA for each call while it
// runs (docs/dev/link.md#who-has-the-flash). The bitstream area can only be erased and
// programmed through the Bitstream calls.

#include <cstddef>
#include <cstdint>

namespace Nor {
void Init();

bool Ready();
bool ReadId(uint8_t id[3]);
bool Read(uint32_t addr, uint8_t *buf, size_t len);
// Erases [addr, addr + len): addr and len multiples of 4 KB; 64 KB blocks where aligned.
bool Erase(uint32_t addr, uint32_t len);
// Programs data (any alignment; split into pages). The range must be erased.
bool Write(uint32_t addr, const uint8_t *data, size_t len);
bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc);

inline constexpr uint32_t BitstreamArea = 0x20000;
bool EraseBitstream(uint32_t len);
bool WriteBitstream(uint32_t addr, const uint8_t *data, size_t len);

void RegisterCommands();

}
