#pragma once

#include <cstddef>
#include <cstdint>

namespace HostLink {
uint32_t Crc32(const void *data, size_t len, uint32_t crc = 0);

}
