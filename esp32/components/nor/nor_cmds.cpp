// nor command group: the FPGA's flash, borrowed from the running FPGA.

#include "host_link.hpp"
#include "nor.hpp"

namespace Nor {

namespace {

using HostLink::Args;
using HostLink::Error;
using HostLink::Reply;

// -> u8 ready, 3 bytes JEDEC ID
void InfoCmd(Args &, Reply &reply)
{
    uint8_t id[3] = {};
    reply.PutU8(ReadId(id));
    reply.Put(id, 3);
}

// u32 addr, u16 len (<= 4096) -> data
void ReadCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint16_t len = args.U16();
    if (args.Bad() || len > 4096) {
        reply.Fail(Error::BadArgs);
        return;
    }
    uint8_t *buf = HostLink::Scratch();
    if (Read(addr, buf, len)) {
        reply.Put(buf, len);
    }
    else {
        reply.Fail(Error::Failed, "read failed");
    }
}

// u32 addr, u32 len (4 KB multiples)
void EraseCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint32_t len = args.U32();
    if (args.Bad() || ((addr | len) & 0xFFF)) {
        reply.Fail(Error::BadArgs, "4 KB multiples");
        return;
    }
    if (!Erase(addr, len)) {
        reply.Fail(Error::Failed, "erase failed (protected?)");
    }
}

// u32 addr, data
void WriteCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const size_t len = args.Remaining();
    const uint8_t *data = args.Bytes(len);
    if (args.Bad() || len > 4096) {
        reply.Fail(Error::BadArgs);
        return;
    }
    if (!Write(addr, data, len)) {
        reply.Fail(Error::Failed, "program failed (protected?)");
    }
}

// u32 addr, u32 len -> u32 CRC-32
void CrcCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint32_t len = args.U32();
    uint32_t crc;
    if (args.Bad()) {
        reply.Fail(Error::BadArgs);
        return;
    }
    if (Crc32(addr, len, &crc)) {
        reply.PutU32(crc);
    }
    else {
        reply.Fail(Error::Failed, "CRC failed");
    }
}

}

void RegisterCommands()
{
    HostLink::Register(HostLink::Cmd::NorInfo, InfoCmd);
    HostLink::Register(HostLink::Cmd::NorRead, ReadCmd);
    HostLink::Register(HostLink::Cmd::NorErase, EraseCmd);
    HostLink::Register(HostLink::Cmd::NorWrite, WriteCmd);
    HostLink::Register(HostLink::Cmd::NorCrc, CrcCmd);
}

}
