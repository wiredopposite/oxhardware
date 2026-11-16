#pragma once

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace ChipUpdate {
// Restores the last good bitstream if the FPGA doesn't configure. Called before anything
// uses the FPGA.
void Start();

bool Handles(OxProg::UploadType type);
bool Active();
OxProg::Status Begin(OxProg::UploadType type, uint32_t size);
OxProg::Status Write(uint32_t offset, const uint8_t *data, size_t len);
OxProg::Status Commit(uint32_t crc);
void Abort();

}
