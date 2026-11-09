#pragma once

#include <cstddef>
#include <cstdint>

namespace HostLink {

// CRC-32 (IEEE 802.3, reflected, as zlib.crc32). Pass the previous result to continue.
uint32_t Crc32(const void *data, size_t len, uint32_t crc = 0);

}
