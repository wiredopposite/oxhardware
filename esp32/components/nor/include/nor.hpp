#pragma once

// The FPGA's flash, over the config bus. With the FPGA running, each call borrows the
// flash from it (docs/01-interface.md §5.5): the FPGA lets go of the pins once the
// console has gone 1 ms without reading it, and answers BIOS reads with 0xFF until the
// call returns. While the FPGA is held in reset the calls use the bus directly.
//
// Borrowed, erase and program below the ROM catalogue (the bitstream) are refused, except
// through the Bitstream calls.

#include <cstddef>
#include <cstdint>

namespace Nor {

void Init();

// The flash can be reached: the FPGA is held, or its design lends the flash.
bool Ready();
bool ReadId(uint8_t id[3]);
bool Read(uint32_t addr, uint8_t *buf, size_t len);
// Erases [addr, addr + len): addr and len multiples of 4 KB; 64 KB blocks where aligned.
bool Erase(uint32_t addr, uint32_t len);
// Programs data (any alignment; split into pages). The range must be erased.
bool Write(uint32_t addr, const uint8_t *data, size_t len);
bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc);

// The bitstream (the FPGA's cold-boot image at 0, docs/01-interface.md §6.1), for the
// chip's own update only: erases [0, len) and programs inside the area, borrowed or held.
inline constexpr uint32_t BitstreamArea = 0x20000;
bool EraseBitstream(uint32_t len);
bool WriteBitstream(uint32_t addr, const uint8_t *data, size_t len);

void RegisterCommands();

}
