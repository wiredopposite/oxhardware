#pragma once

#include <cstddef>
#include <cstdint>

#include "oxprog/oxprog.hpp"

namespace RomMgr {
void Start();

OxProg::Status Rom(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                   size_t &out_len);
OxProg::Status Upload(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                      size_t &out_len);
OxProg::Status Download(uint8_t action, const uint8_t *in, size_t len, uint8_t *out,
                        size_t &out_len);
OxProg::Status Boot(const OxProg::BootRequest &req);
OxProg::Status ConfirmBoot();

void FillInfo(OxProg::Info &info);

}
