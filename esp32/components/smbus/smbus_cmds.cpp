// smbus command group.

#include "host_link.hpp"
#include "smbus.hpp"

namespace Smbus {

namespace {

using HostLink::Args;
using HostLink::Error;
using HostLink::Reply;

// u8 addr, u8 reg, u8 width (1/2) -> value (width bytes)
void ReadCmd(Args &args, Reply &reply)
{
    const uint8_t addr = args.U8();
    const uint8_t reg = args.U8();
    const uint8_t width = args.U8();
    if (args.Bad() || (width != 1 && width != 2)) {
        reply.Fail(Error::BadArgs);
        return;
    }
    bool ok;
    if (width == 1) {
        uint8_t v;
        ok = ReadByte(addr, reg, &v);
        reply.PutU8(v);
    }
    else {
        uint16_t v;
        ok = ReadWord(addr, reg, &v);
        reply.PutU16(v);
    }
    if (!ok) {
        reply.Fail(Error::Failed, "no ACK (console off, or wiring)");
    }
}

// u8 addr, u8 reg, u8 width (1/2), value (width bytes)
void WriteCmd(Args &args, Reply &reply)
{
    const uint8_t addr = args.U8();
    const uint8_t reg = args.U8();
    const uint8_t width = args.U8();
    const uint16_t val = width == 2 ? args.U16() : args.U8();
    if (args.Bad() || (width != 1 && width != 2)) {
        reply.Fail(Error::BadArgs);
        return;
    }
    const bool ok = width == 1 ? WriteByte(addr, reg, uint8_t(val)) : WriteWord(addr, reg, val);
    if (!ok) {
        reply.Fail(Error::Failed, "no ACK (console off, or wiring)");
    }
}

// u8 action (Power)
void PowerCmd(Args &args, Reply &reply)
{
    const uint8_t a = args.U8();
    if (args.Bad() || (a != 0x01 && a != 0x40 && a != 0x80)) {
        reply.Fail(Error::BadArgs);
        return;
    }
    if (!SmcPower(Power(a))) {
        reply.Fail(Error::Failed, "SMC did not ACK");
    }
}

}

void RegisterCommands()
{
    HostLink::Register(HostLink::Cmd::SmbusRead, ReadCmd);
    HostLink::Register(HostLink::Cmd::SmbusWrite, WriteCmd);
    HostLink::Register(HostLink::Cmd::SmcPower, PowerCmd);
}

}
