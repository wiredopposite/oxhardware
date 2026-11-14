#pragma once

// The chip's own updates through the programmer protocol (docs/07-console-protocol.md):
// the FPGA bitstream and the ESP32 firmware, uploaded like ROMs (Begin, Write, Commit).
//
// Bitstream: staged in the ESP32's `golden` partition, written to the FPGA's flash with
// the FPGA running (borrowed), then the FPGA is reloaded; the console keeps running. If
// the new design doesn't come up, the last good bitstream is written back. That copy is
// also what the FPGA gets at boot when it doesn't configure (a write cut off).

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace ChipUpdate {

// At boot, before anything uses the FPGA: restores the last good bitstream if the FPGA
// doesn't configure
void Start();

bool Handles(OxProg::UploadType type);
bool Active();
OxProg::Status Begin(OxProg::UploadType type, uint32_t size);
OxProg::Status Write(uint32_t offset, const uint8_t *data, size_t len);
OxProg::Status Commit(uint32_t crc);
void Abort();

}
