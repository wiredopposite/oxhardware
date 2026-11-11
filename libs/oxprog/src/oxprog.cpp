#include <array>
#include "oxprog/oxprog.hpp"

namespace OxProg {

namespace {

constexpr std::array<uint32_t, 256> MakeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto CrcTable = MakeCrcTable();

}

uint32_t Crc32(const void *Data, size_t Length, uint32_t Seed)
{
    const auto *bytes = static_cast<const uint8_t *>(Data);
    uint32_t crc = ~Seed;
    while (Length--) {
        crc = CrcTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

const char *StatusName(Status Status)
{
    switch (Status) {
    case Status::Ok: return "ok";
    case Status::ConsoleOn: return "the console is on, power it off first";
    case Status::ProgrammerBusy: return "programmer busy";
    case Status::LayoutInvalid: return "flash layout invalid";
    case Status::NotStarted: return "no transfer started";
    case Status::BadSize: return "bad size";
    case Status::BadOffset: return "bad offset";
    case Status::BadCommand: return "command not supported";
    case Status::FlashError: return "flash error";
    case Status::CrcMismatch: return "CRC mismatch";
    case Status::NotFound: return "not found";
    case Status::Protected: return "protected";
    case Status::NoSpace: return "no free slot";
    case Status::ConsoleOff: return "the console is off";
    case Status::SmbusError: return "no SMBus response";
    case Status::Fragmented: return "no free slot until compacted";
    case Status::Timeout: return "no reply";
    case Status::TransportError: return "transport error";
    }
    return "unknown status";
}

}
