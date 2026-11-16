#pragma once

#include <cstddef>
#include <cstdint>

namespace HostLink {
enum class FrameType : uint8_t {
    Request = 0x01,
    Ok = 0x02,
    Err = 0x03,
    Event = 0x04,
    Log = 0x05,
};

enum class Error : uint8_t {
    UnknownCommand = 0x01,
    BadArgs = 0x02,
    Busy = 0x03,
    Failed = 0x04,
};

inline constexpr size_t MaxPayload = 4096 + 64;

class Reply {
public:
    bool Put(const void *data, size_t len);
    bool PutU8(uint8_t v) { return Put(&v, 1); }
    bool PutU16(uint16_t v);
    bool PutU32(uint32_t v);
    bool PutU64(uint64_t v);
    bool PutStr(const char *s);       // length-prefixed (1 byte), truncated to 255
    void Fail(Error code, const char *message = nullptr);
    void Reset();

    const uint8_t *Data() const { return Buf; }
    size_t Size() const { return Len; }
    bool Failed() const { return HasError; }
    Error Code() const { return ErrCode; }

private:
    uint8_t Buf[MaxPayload - 2];
    size_t Len = 0;
    bool HasError = false;
    Error ErrCode = Error::Failed;
};

class Args {
public:
    Args(const uint8_t *data, size_t len) : Ptr(data), Left(len) {}
    uint8_t U8();
    uint16_t U16();
    uint32_t U32();
    const uint8_t *Bytes(size_t n);
    size_t Remaining() const { return Left; }
    bool Bad() const { return Overrun; }

private:
    const uint8_t *Ptr;
    size_t Left;
    bool Overrun = false;
};

using Handler = void (*)(Args &args, Reply &reply);

namespace Cmd {
inline constexpr uint16_t SysPing = 0x0000;
inline constexpr uint16_t SysVersion = 0x0001;
inline constexpr uint16_t SysStatus = 0x0002;
inline constexpr uint16_t SysReboot = 0x0003;
inline constexpr uint16_t LogLevel = 0x0100;
inline constexpr uint16_t FpgaInfo = 0x0200;
inline constexpr uint16_t FpgaHold = 0x0201;
inline constexpr uint16_t FpgaRelease = 0x0202;
inline constexpr uint16_t FlashId = 0x0203;
inline constexpr uint16_t FlashRead = 0x0204;
inline constexpr uint16_t FlashErase = 0x0205;
inline constexpr uint16_t FlashWrite = 0x0206;
inline constexpr uint16_t FlashCrc = 0x0207;
inline constexpr uint16_t FlashProbe = 0x0208;
inline constexpr uint16_t LinkRead = 0x0300;
inline constexpr uint16_t LinkWrite = 0x0301;
inline constexpr uint16_t LinkProbe = 0x0302;
inline constexpr uint16_t LinkClock = 0x0303;
inline constexpr uint16_t LinkIrq = 0x0304;
inline constexpr uint16_t SmbusRead = 0x0400;
inline constexpr uint16_t SmbusWrite = 0x0401;
inline constexpr uint16_t SmcPower = 0x0402;
inline constexpr uint16_t ProgXfer = 0x0500;
inline constexpr uint16_t NorInfo = 0x0600;
inline constexpr uint16_t NorRead = 0x0601;
inline constexpr uint16_t NorErase = 0x0602;
inline constexpr uint16_t NorWrite = 0x0603;
inline constexpr uint16_t NorCrc = 0x0604;
}

void Start();
void Register(uint16_t cmd, Handler handler);

// Shared by command handlers, which run one at a time
inline constexpr size_t ScratchSize = 4096 + 8;
uint8_t *Scratch();
void SendEvent(uint16_t id, const void *data, size_t len);

}
