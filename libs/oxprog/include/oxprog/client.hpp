#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
#include "oxprog/oxprog.hpp"

namespace OxProg {

// A byte stream to the chip: the LPC console port from the console, a serial
// port from a PC
class Transport {
public:
    virtual ~Transport() = default;

    // Everything is written or it's a failure
    virtual bool Write(const void *Data, size_t Length) = 0;
    // Whatever arrives within TimeoutMs, up to Length. 0 on timeout.
    virtual size_t Read(void *Data, size_t Length, uint32_t TimeoutMs) = 0;
    // Drops anything received but not read, to resync after an error
    virtual void Discard() { }
};

// Done and total bytes, for progress
using Progress = std::function<void(uint32_t Done, uint32_t Total)>;

struct UploadOptions {
    char     Name[NameMax + 1] = {};
    uint16_t Version = 1;
    uint32_t SourceCrc32 = 0;
    uint8_t  Flags = 0;     // CommitFlags
    bool     Force = false; // BeginForce
};

// Commands are sent one at a time, each waits for its reply
class Client {
public:
    explicit Client(Transport &Transport, uint32_t TimeoutMs = 5000)
        : Link(Transport), TimeoutMs(TimeoutMs) { }

    Status GetInfo(Info &Out);
    Status Reboot();
    Status SmbusRead(uint8_t Address, uint8_t Command, uint8_t &Value);
    Status SmbusWrite(uint8_t Address, uint8_t Command, uint8_t Value);
    // Arms Entry for the console's next boot, with BootReset also resets it
    Status Boot(uint8_t Entry, uint8_t Flags = BootReset);
    Status ConfirmBoot();

    // The whole catalogue
    Status ListRoms(std::vector<RomEntry> &Out);
    Status GetRom(uint8_t Entry, RomEntry &Out);
    Status DeleteRom(uint8_t Entry);
    Status RenameRom(uint8_t Entry, const char *Name);
    // NoEntry for none
    Status SetUserRom(uint8_t Entry);
    // Moves entries until a run of ImageSize is free (after an upload's Fragmented)
    Status CompactRoms(uint32_t ImageSize);

    // Begin, the image in chunks, then commit with its CRC. Aborted on the
    // chip if anything fails part way. For the ROM types Entry (if given)
    // gets the new catalogue entry.
    Status Upload(UploadType Type, std::span<const uint8_t> Image,
        const UploadOptions &Options = {}, const Progress &OnProgress = {},
        RomEntry *Entry = nullptr);
    Status AbortUpload();

    // The whole image, checked against the CRC the chip reports. Entry is
    // for DownloadType::Rom.
    Status Download(DownloadType Type, std::vector<uint8_t> &Out,
        uint8_t Entry = NoEntry, const Progress &OnProgress = {});

    // How long a command waits for its reply. Committing to flash takes
    // seconds, so that one is always given longer.
    void SetTimeout(uint32_t Ms) { TimeoutMs = Ms; }

private:
    static constexpr uint32_t CommitTimeoutMs = 60'000;

    Transport &Link;
    uint32_t TimeoutMs;
    std::vector<uint8_t> Reply;

    Status Transact(Mode Mode, uint8_t Action, const void *Payload, uint16_t Length,
        uint32_t Timeout = 0);
    bool ReadExact(void *Data, size_t Length, uint32_t Timeout);
    bool ReadReply(Command Cmd, uint32_t Timeout, Status &Result);
    // Copies the reply into Out, zero-filling what an older chip didn't send
    template <typename T> void TakeReply(T &Out);
};

}
