#include "cfg_bus.hpp"
#include "host_link.hpp"

namespace CfgBus {
namespace {
using HostLink::Args;
using HostLink::Error;
using HostLink::Reply;

bool NeedHeld(Reply &reply)
{
    if (!Held()) {
        reply.Fail(Error::Failed, "FPGA not held (fpga hold first)");
        return false;
    }
    return true;
}

void Info(Args &, Reply &reply)
{
    reply.PutU8(Cdone());
    reply.PutU8(Held());
}

void HoldCmd(Args &, Reply &reply)
{
    if (!Hold()) {
        reply.Fail(Error::Failed, "could not take the config bus");
    }
}

void ReleaseCmd(Args &args, Reply &reply)
{
    uint32_t timeout = args.Remaining() ? args.U32() : 1000;
    reply.PutU32(uint32_t(int32_t(Release(timeout))));
}

void IdCmd(Args &, Reply &reply)
{
    uint8_t id[3];
    if (NeedHeld(reply)) {
        if (ReadId(id)) {
            reply.Put(id, 3);
        }
        else {
            reply.Fail(Error::Failed, "SPI error");
        }
    }
}

void ReadCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint16_t len = args.U16();
    if (args.Bad() || len > 4096) {
        reply.Fail(Error::BadArgs);
        return;
    }
    uint8_t *buf = HostLink::Scratch();
    if (NeedHeld(reply)) {
        if (Read(addr, buf, len)) {
            reply.Put(buf, len);
        }
        else {
            reply.Fail(Error::Failed, "read failed");
        }
    }
}

void EraseCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint32_t len = args.U32();
    if (args.Bad() || (addr & 0xFFF) || (len & 0xFFF)) {
        reply.Fail(Error::BadArgs, "address and length must be 4 KB multiples");
        return;
    }
    if (NeedHeld(reply) && !Erase(addr, len)) {
        reply.Fail(Error::Failed, "erase failed");
    }
}

void WriteCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const size_t len = args.Remaining();
    const uint8_t *data = args.Bytes(len);
    if (args.Bad() || len > 4096) {
        reply.Fail(Error::BadArgs);
        return;
    }
    if (NeedHeld(reply) && !Write(addr, data, len)) {
        reply.Fail(Error::Failed, "program failed");
    }
}

void CrcCmd(Args &args, Reply &reply)
{
    const uint32_t addr = args.U32();
    const uint32_t len = args.U32();
    if (args.Bad()) {
        return;
    }
    uint32_t crc;
    if (NeedHeld(reply)) {
        if (Crc32(addr, len, &crc)) {
            reply.PutU32(crc);
        }
        else {
            reply.Fail(Error::Failed, "read failed");
        }
    }
}

void ProbeCmd(Args &, Reply &reply)
{
    uint8_t id[3] = {};
    const uint8_t idle = Probe(id);
    if (idle == 0xFF) {
        reply.Fail(Error::Busy, "release the FPGA first");
        return;
    }
    reply.PutU8(idle);
    reply.Put(id, 3);
}

}

void RegisterCommands()
{
    HostLink::Register(HostLink::Cmd::FlashProbe, ProbeCmd);
    HostLink::Register(HostLink::Cmd::FpgaInfo, Info);
    HostLink::Register(HostLink::Cmd::FpgaHold, HoldCmd);
    HostLink::Register(HostLink::Cmd::FpgaRelease, ReleaseCmd);
    HostLink::Register(HostLink::Cmd::FlashId, IdCmd);
    HostLink::Register(HostLink::Cmd::FlashRead, ReadCmd);
    HostLink::Register(HostLink::Cmd::FlashErase, EraseCmd);
    HostLink::Register(HostLink::Cmd::FlashWrite, WriteCmd);
    HostLink::Register(HostLink::Cmd::FlashCrc, CrcCmd);
}

}
