// link command group: raw access to the FPGA's link address spaces.

#include "fpga_link.hpp"
#include "host_link.hpp"

namespace FpgaLink {

namespace {

using HostLink::Args;
using HostLink::Error;
using HostLink::Reply;

// u8 space (bit3 = noinc), u32 addr, u16 len -> u8 STS, data
void ReadCmd(Args &args, Reply &reply)
{
    const uint8_t space = args.U8();
    const uint32_t addr = args.U32();
    const uint16_t len = args.U16();
    if (args.Bad() || len > MaxXfer || (space & 7) > 3) {
        reply.Fail(Error::BadArgs);
        return;
    }
    static uint8_t buf[MaxXfer];
    uint8_t sts;
    if (!Read(Space(space & 7), addr, buf, len, &sts, space & 8)) {
        reply.Fail(Error::Failed, "SPI error");
        return;
    }
    reply.PutU8(sts);
    reply.Put(buf, len);
}

// u8 space (bit3 = noinc), u32 addr, data -> u8 STS
void WriteCmd(Args &args, Reply &reply)
{
    const uint8_t space = args.U8();
    const uint32_t addr = args.U32();
    const size_t len = args.Remaining();
    const uint8_t *data = args.Bytes(len);
    if (args.Bad() || len > MaxXfer || (space & 7) > 3) {
        reply.Fail(Error::BadArgs);
        return;
    }
    uint8_t sts;
    if (!Write(Space(space & 7), addr, data, len, &sts, space & 8)) {
        reply.Fail(Error::Failed, "SPI error");
        return;
    }
    reply.PutU8(sts);
}

// u32 Hz
void ClockCmd(Args &args, Reply &reply)
{
    const uint32_t hz = args.U32();
    if (args.Bad() || hz < 100000 || hz > 20000000) {
        reply.Fail(Error::BadArgs, "0.1 to 20 MHz");
        return;
    }
    SetClock(int(hz));
}

// -> u8 MISO levels, u8 STS, 4 bytes ID (bit-banged)
void ProbeCmd(Args &, Reply &reply)
{
    uint8_t rx[6];
    reply.PutU8(Probe(rx));
    reply.Put(rx, 5);
}

}

void RegisterCommands()
{
    HostLink::Register(HostLink::Cmd::LinkProbe, ProbeCmd);
    HostLink::Register(HostLink::Cmd::LinkClock, ClockCmd);
    HostLink::Register(HostLink::Cmd::LinkRead, ReadCmd);
    HostLink::Register(HostLink::Cmd::LinkWrite, WriteCmd);
}

}
