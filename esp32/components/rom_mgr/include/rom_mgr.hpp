#pragma once

// ROM library in the FPGA's flash (docs/03-esp32.md §5): the catalogue, slot allocation,
// uploads and downloads, and arming entries for a boot. The programmer protocol's Rom,
// Upload and Download modes and its Boot command (docs/07-console-protocol.md §3).
//
// Handlers take a request payload and write the reply payload to out (at most
// OxProg::PayloadMax bytes), setting out_len. Safe to call from any task.

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace RomMgr {

// Loads the catalogue and starts the task that watches trial boots (docs/06-boot-flow.md §7)
void Start();

OxProg::Status Rom(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                   size_t &out_len);
OxProg::Status Upload(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                      size_t &out_len);
OxProg::Status Download(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                        size_t &out_len);
OxProg::Status Boot(const OxProg::BootRequest &req);
// The console's System confirming the trial boot it is running
OxProg::Status ConfirmBoot();

// The catalogue fields of GetInfo
void FillInfo(OxProg::Info &info);

}
