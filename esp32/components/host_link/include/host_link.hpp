#pragma once

// PC protocol on UART0 (docs/03-esp32.md §12).
//
// Wire: COBS-encoded frames, each preceded and followed by 0x00. Decoded frame:
//   type (1) | seq (1) | len (2, LE) | payload (len) | CRC-32 (4, LE, over type..payload)
// Requests carry cmd (2, LE) + arguments. Every request gets exactly one response with
// the same seq: OK = cmd + data, ERR = cmd + error code (1) + message.

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

// Response under construction. Handlers append data; the command code is added by
// HostLink.
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

// Argument reader with bounds checks; any overrun sets Bad().
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

// Command codes: high byte = group, low byte = command.
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
inline constexpr uint16_t SmbusRead = 0x0400;
inline constexpr uint16_t SmbusWrite = 0x0401;
inline constexpr uint16_t SmcPower = 0x0402;
}

// Takes over UART0 at the console baud rate, routes ESP-IDF logging into Log frames and
// starts the receive task. Register handlers before or after; requests for unregistered
// commands are answered UnknownCommand.
void Start();
void Register(uint16_t cmd, Handler handler);
void SendEvent(uint16_t id, const void *data, size_t len);

}
