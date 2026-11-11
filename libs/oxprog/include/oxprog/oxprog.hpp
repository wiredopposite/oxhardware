#pragma once

// The OpenOX chip's programmer protocol (docs/07-console-protocol.md): packets
// between a host (the console through LPC I/O ports, or a PC) and the chip's
// ESP32. Everything here is shared by both ends.

#include <cstddef>
#include <cstdint>

namespace OxProg {

// "OXFP" in byte order
inline constexpr uint32_t Magic = 0x5046584F;
inline constexpr uint16_t Version = 3;
inline constexpr uint16_t PayloadMax = 4 * 1024;
inline constexpr uint32_t NameMax = 32;

// The console's way to the programmer: the same packets, a byte at a time
// through LPC I/O ports. The counts never have bit 7 set, so a read of 0xFF,
// what the console gets when nothing answers, is never taken for one.
namespace ConsolePort {

inline constexpr uint16_t Base = 0x0B00;
inline constexpr uint16_t Mask = 0xFFFC;
// Reads the next byte of a reply, writes the next byte of a command
inline constexpr uint16_t Data = Base + 0;
// How many bytes Data has to read, at most CountMax
inline constexpr uint16_t Available = Base + 1;
// How many bytes Data takes, at most CountMax
inline constexpr uint16_t Space = Base + 2;
// Reads IdReady, or IdResetting until a reset is done and the programmer is
// up. Writing IdReset drops whatever is queued either way and ends any
// transfer the console started.
inline constexpr uint16_t Id = Base + 3;

inline constexpr uint8_t CountMax = 0x7F;
inline constexpr uint8_t IdReady = 'O';
inline constexpr uint8_t IdResetting = 'R';
inline constexpr uint8_t IdReset = 0x01;

}

enum class Status : uint8_t {
    Ok = 0,
    ConsoleOn,
    ProgrammerBusy,
    LayoutInvalid,
    NotStarted,
    BadSize,
    BadOffset,
    BadCommand,
    FlashError,
    CrcMismatch,
    // Nothing of that type is stored, or no such entry
    NotFound,
    // The slot or entry belongs to a protected role
    Protected,
    // No free slots of the size asked for
    NoSpace,
    // Needs the console on
    ConsoleOff,
    // Nothing acked on the SMBus
    SmbusError,
    // No free slots of the size asked for, but RomAction::Compact can make some
    Fragmented,

    // Reported by the client, never sent by the chip
    Timeout = 0xFE,
    TransportError = 0xFF
};

enum class Mode : uint8_t {
    None,
    Control,
    Upload,
    Download,
    Rom,
    Count
};
static_assert(uint8_t(Mode::Count) <= 0x10);

enum class ControlAction : uint8_t {
    // Replies Info
    GetInfo,
    // Restarts the ESP32; the reply is sent first
    Reboot,
    // SmbusTransfer, replies SmbusTransfer with the value read
    SmbusRead,
    // SmbusTransfer
    SmbusWrite,
    // BootRequest: arms a catalogue entry for the console's next boot
    Boot,
    // Confirms the boot running now (a trial System, docs/06-boot-flow §7)
    ConfirmBoot
};

enum class RomAction : uint8_t {
    // RomList, replies as many RomEntry as fit from First on
    List,
    // RomRef, replies RomEntry
    Get,
    // RomRef
    Delete,
    // RomRename
    Rename,
    // RomRef, 0xFF for none: what a power-button boot runs
    SetUser,
    // RomCompact, replies RomCompactStep: moves one entry towards a free run of
    // ImageSize. Repeat until Moved is NoEntry.
    Compact
};

enum class UploadAction : uint8_t {
    // UploadBegin
    Begin,
    // UploadWrite
    Write,
    // UploadCommit, replies RomEntry for the ROM types
    Commit,
    Abort
};

enum class DownloadAction : uint8_t {
    // DownloadBegin, replies DownloadInfo
    Begin,
    // DownloadRead, replies the data
    Read,
    End
};

// What a ROM in the catalogue is for (docs/06-boot-flow §1)
enum class Role : uint8_t {
    User,
    Loader,
    System,
    Diag,
    Count
};

enum class UploadType : uint8_t {
    // Data the System BIOS is built from, kept for the console
    RetailTables,
    // ROMs, into a free slot of their role
    UserRom,
    LoaderRom,
    SystemRom,
    DiagRom,
    // The chip's own firmware
    Esp32Firmware,
    FpgaBitstream,
    Count
};

enum class DownloadType : uint8_t {
    RetailTables,
    // A catalogue entry, DownloadBegin::Entry
    Rom,
    // What the console is being served
    ActiveRom,
    Count
};

// UploadBegin::Flags
enum BeginFlags : uint8_t {
    // Write with the console running a BIOS that isn't the one replaced.
    // Always allowed from the console itself for user ROMs and spare slots.
    BeginForce = 0x01
};

// UploadCommit::Flags
enum CommitFlags : uint8_t {
    // Loader, System: boot once on trial, current only after it confirms.
    // Without it the image is current straight away, what a developer
    // uploading a build wants.
    CommitTrial = 0x01,
    // UserRom: make it the power-button ROM
    CommitUser = 0x02,
    // Boot it from the console's next reset
    CommitBoot = 0x04
};

// RomEntry::Flags
enum RomFlags : uint8_t {
    // The role's copy in use: the confirmed Loader or System
    RomCurrent = 0x01,
    // Written, not yet confirmed
    RomTrial = 0x02,
    // Its last armed boot failed (docs/06-boot-flow §6)
    RomFailedBoot = 0x04,
    // The power-button ROM
    RomUser = 0x08
};

// BootRequest::Flags
enum BootFlags : uint8_t {
    // Reset the console through the SMC once armed. Without it the console
    // resets itself.
    BootReset = 0x01
};

// Info::BootFlags, the FPGA's BOOT_FLAGS (docs/06-boot-flow §4)
enum FpgaBootFlags : uint8_t {
    FpgaArmed = 0x01,
    FpgaArmedBoot = 0x02,
    FpgaTrial = 0x04,
    FpgaLastFailed = 0x08,
    FpgaConfirmed = 0x10
};

inline constexpr uint8_t NoEntry = 0xFF;
inline constexpr uint8_t NoBank = 0xFF;

// A bank as the FPGA serves it: bits 5:0 the slot (256 K units from the ROM
// area), bits 7:6 the size
inline constexpr uint32_t SlotSize = 256 * 1024;
inline constexpr uint8_t BankSlot(uint8_t Bank) { return Bank & 0x3F; }
inline constexpr uint32_t BankSize(uint8_t Bank) { return SlotSize << (Bank >> 6); }
inline constexpr uint8_t MakeBank(uint8_t Slot, uint32_t Size)
{
    return uint8_t((Slot & 0x3F) | ((Size >= 4 * SlotSize ? 2 : Size > SlotSize ? 1 : 0) << 6));
}

#pragma pack(push, 1)

// Bits 3:0 the mode, 7:4 the action
struct Command {
    uint8_t Value;

    static constexpr Command Make(OxProg::Mode Mode, uint8_t Action)
    {
        return { uint8_t((uint8_t(Mode) & 0x0F) | (Action << 4)) };
    }
    constexpr OxProg::Mode Mode() const { return OxProg::Mode(Value & 0x0F); }
    constexpr uint8_t Action() const { return Value >> 4; }
};

struct Header {
    uint32_t Magic;
    Command  Cmd;
    Status   Result;
    uint16_t Length;
    uint32_t PayloadCrc32;
    uint32_t HeaderCrc32;
};
static_assert(sizeof(Header) == 16);

struct Info {
    uint16_t Version;
    uint8_t  FirmwareVersion[3];
    uint8_t  FpgaVersion[3];
    uint8_t  BoardId;
    uint8_t  ConsolePowered;
    // FpgaBootFlags
    uint8_t  BootFlags;
    // Catalogue entries, NoEntry for none
    uint8_t  ActiveEntry;
    uint8_t  ArmedEntry;
    uint8_t  UserEntry;
    uint8_t  LoaderEntry;
    uint8_t  SystemEntry;
    uint8_t  DiagEntry;
    uint8_t  EntryCount;
    uint8_t  SlotCount;
    uint8_t  FreeSlots;
    uint32_t FlashSize;
    // RetailTables stored, 0 if not
    uint32_t RetailTablesSize;
    uint32_t RetailTablesSourceCrc32;
};
static_assert(sizeof(Info) <= PayloadMax);

struct RomEntry {
    uint8_t  Id;
    Role     Kind;
    // Bank format above
    uint8_t  Bank;
    // RomFlags
    uint8_t  Flags;
    uint32_t Size;
    uint32_t Crc32;
    // What the image was made from, e.g. the CRC of the retail kernel the
    // retail tables came from. Chosen by whoever uploads it.
    uint32_t SourceCrc32;
    uint16_t Version;
    uint16_t Reserved;
    char     Name[NameMax + 1];
};
inline constexpr size_t RomEntriesMax = PayloadMax / sizeof(RomEntry);

struct SmbusTransfer {
    // 7-bit address
    uint8_t Address;
    uint8_t Command;
    uint8_t Value;
    uint8_t Reserved;
};

inline constexpr uint8_t SmcAddress = 0x10;
inline constexpr uint8_t SmcPowerRegister = 0x02;
inline constexpr uint8_t SmcPowerReset = 0x01;
inline constexpr uint8_t SmcPowerCycle = 0x40;
inline constexpr uint8_t SmcPowerOff = 0x80;

struct BootRequest {
    uint8_t Entry;
    // BootFlags
    uint8_t Flags;
};

struct RomList {
    uint8_t First;
};

struct RomRef {
    uint8_t Entry;
};

struct RomRename {
    uint8_t Entry;
    char    Name[NameMax + 1];
};

struct RomCompact {
    uint32_t ImageSize;
};

struct RomCompactStep {
    // The entry moved, NoEntry once a run of ImageSize is free
    uint8_t Moved;
    // Moves still to go after this one
    uint8_t Remaining;
};

struct UploadBegin {
    UploadType Type;
    // BeginFlags
    uint8_t    Flags;
    uint16_t   Reserved;
    uint32_t   ImageSize;
    char       Name[NameMax + 1];
};

struct UploadWrite {
    uint32_t Offset;
    uint8_t  Data[1];
};
inline constexpr uint16_t UploadWriteMax = PayloadMax - offsetof(UploadWrite, Data);

struct UploadCommit {
    uint32_t Crc32;
    uint32_t SourceCrc32;
    uint16_t Version;
    // CommitFlags
    uint8_t  Flags;
    uint8_t  Reserved;
};

struct DownloadBegin {
    DownloadType Type;
    uint8_t      Entry;
};

struct DownloadInfo {
    uint32_t Size;
    uint32_t Crc32;
};

struct DownloadRead {
    uint32_t Offset;
    uint32_t Length;
};
inline constexpr uint16_t DownloadReadMax = PayloadMax;

#pragma pack(pop)

// CRC-32 IEEE (zlib), as used for every CRC in the protocol. Seed is a
// previous result, to continue over several buffers.
uint32_t Crc32(const void *Data, size_t Length, uint32_t Seed = 0);

const char *StatusName(Status Status);

}
