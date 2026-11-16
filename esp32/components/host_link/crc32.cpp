#include "crc32.hpp"

namespace HostLink {
namespace {
struct Table {
    uint32_t v[256];
    constexpr Table() : v()
    {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Table CrcTable;

}

uint32_t Crc32(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    crc = ~crc;
    while (len--) {
        crc = CrcTable.v[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

}
