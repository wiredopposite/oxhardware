#include <algorithm>
#include <cstring>
#include "oxprog/client.hpp"

namespace OxProg {
bool Client::ReadExact(void *Data, size_t Length, uint32_t Timeout)
{
    auto *out = static_cast<uint8_t *>(Data);
    while (Length) {
        const size_t got = Link.Read(out, Length, Timeout);
        if (!got) {
            return false;
        }
        out += got;
        Length -= got;
    }
    return true;
}

// Skips anything before the magic, the device only ever sends replies
bool Client::ReadReply(Command Cmd, uint32_t Timeout, Status &Result)
{
    Header header{};
    auto *bytes = reinterpret_cast<uint8_t *>(&header);

    size_t have = 0;
    while (have < sizeof(header.Magic)) {
        if (!ReadExact(bytes + have, 1, Timeout)) {
            Result = Status::Timeout;
            return false;
        }
        have++;
        if (have == sizeof(header.Magic) && header.Magic != Magic) {
            std::memmove(bytes, bytes + 1, --have);
        }
    }
    if (!ReadExact(bytes + have, sizeof(header) - have, Timeout)) {
        Result = Status::Timeout;
        return false;
    }

    if (header.HeaderCrc32 != Crc32(&header, offsetof(Header, HeaderCrc32)) ||
        header.Length > PayloadMax) {
        Result = Status::TransportError;
        return false;
    }

    Reply.resize(header.Length);
    if (header.Length && !ReadExact(Reply.data(), header.Length, Timeout)) {
        Result = Status::Timeout;
        return false;
    }
    if (header.PayloadCrc32 != (header.Length ? Crc32(Reply.data(), header.Length) : 0) ||
        header.Cmd.Value != Cmd.Value) {
        Result = Status::TransportError;
        return false;
    }

    Result = header.Result;
    return true;
}

Status Client::Transact(Mode Mode, uint8_t Action, const void *Payload, uint16_t Length,
    uint32_t Timeout)
{
    if (Length > PayloadMax) {
        return Status::BadSize;
    }

    const auto cmd = Command::Make(Mode, Action);

    Header header{};
    header.Magic = Magic;
    header.Cmd = cmd;
    header.Result = Status::Ok;
    header.Length = Length;
    header.PayloadCrc32 = Length ? Crc32(Payload, Length) : 0;
    header.HeaderCrc32 = Crc32(&header, offsetof(Header, HeaderCrc32));

    if (!Link.Write(&header, sizeof(header)) || (Length && !Link.Write(Payload, Length))) {
        return Status::TransportError;
    }

    Status result;
    if (!ReadReply(cmd, Timeout ? Timeout : TimeoutMs, result)) {
        // Whatever's left of the bad reply would be read as the next one
        Link.Discard();
        Reply.clear();
    }
    return result;
}

// An older device sends less, a newer one more
template <typename T> void Client::TakeReply(T &Out)
{
    std::memset(&Out, 0, sizeof(Out));
    std::memcpy(&Out, Reply.data(), std::min(Reply.size(), sizeof(Out)));
}

Status Client::GetInfo(Info &Out)
{
    const auto status = Transact(Mode::Control, uint8_t(ControlAction::GetInfo), nullptr, 0);
    if (status == Status::Ok) {
        TakeReply(Out);
    }
    return status;
}

Status Client::Reboot()
{
    return Transact(Mode::Control, uint8_t(ControlAction::Reboot), nullptr, 0);
}

Status Client::SmbusRead(uint8_t Address, uint8_t Command, uint8_t &Value)
{
    SmbusTransfer xfer{};
    xfer.Address = Address;
    xfer.Command = Command;
    const auto status = Transact(Mode::Control, uint8_t(ControlAction::SmbusRead), &xfer,
        sizeof(xfer));
    if (status == Status::Ok) {
        TakeReply(xfer);
        Value = xfer.Value;
    }
    return status;
}

Status Client::SmbusWrite(uint8_t Address, uint8_t Command, uint8_t Value)
{
    SmbusTransfer xfer{};
    xfer.Address = Address;
    xfer.Command = Command;
    xfer.Value = Value;
    return Transact(Mode::Control, uint8_t(ControlAction::SmbusWrite), &xfer, sizeof(xfer));
}

Status Client::Boot(uint8_t Entry, uint8_t Flags)
{
    BootRequest req{ Entry, Flags };
    return Transact(Mode::Control, uint8_t(ControlAction::Boot), &req, sizeof(req));
}

Status Client::ConfirmBoot()
{
    return Transact(Mode::Control, uint8_t(ControlAction::ConfirmBoot), nullptr, 0);
}

Status Client::ListRoms(std::vector<RomEntry> &Out)
{
    Out.clear();
    for (;;) {
        RomList list{ static_cast<uint8_t>(Out.size()) };
        const auto status = Transact(Mode::Rom, uint8_t(RomAction::List), &list, sizeof(list));
        if (status != Status::Ok) {
            Out.clear();
            return status;
        }
        const size_t count = Reply.size() / sizeof(RomEntry);
        const size_t start = Out.size();
        Out.resize(start + count);
        std::memcpy(Out.data() + start, Reply.data(), count * sizeof(RomEntry));
        if (count < RomEntriesMax) {
            return Status::Ok;
        }
    }
}

Status Client::GetRom(uint8_t Entry, RomEntry &Out)
{
    RomRef ref{ Entry };
    const auto status = Transact(Mode::Rom, uint8_t(RomAction::Get), &ref, sizeof(ref));
    if (status == Status::Ok) {
        TakeReply(Out);
    }
    return status;
}

Status Client::DeleteRom(uint8_t Entry)
{
    RomRef ref{ Entry };
    return Transact(Mode::Rom, uint8_t(RomAction::Delete), &ref, sizeof(ref));
}

Status Client::RenameRom(uint8_t Entry, const char *Name)
{
    RomRename rename{};
    rename.Entry = Entry;
    std::memcpy(rename.Name, Name, std::min<size_t>(std::strlen(Name), NameMax));
    return Transact(Mode::Rom, uint8_t(RomAction::Rename), &rename, sizeof(rename));
}

Status Client::SetUserRom(uint8_t Entry)
{
    RomRef ref{ Entry };
    return Transact(Mode::Rom, uint8_t(RomAction::SetUser), &ref, sizeof(ref));
}

Status Client::CompactRoms(uint32_t ImageSize)
{
    RomCompact compact{ ImageSize };
    for (;;) {
        const auto status = Transact(Mode::Rom, uint8_t(RomAction::Compact), &compact,
                                     sizeof(compact), CommitTimeoutMs);
        if (status != Status::Ok) {
            return status;
        }
        RomCompactStep step{};
        TakeReply(step);
        if (step.Moved == NoEntry) {
            return Status::Ok;
        }
    }
}

Status Client::AbortUpload()
{
    return Transact(Mode::Upload, uint8_t(UploadAction::Abort), nullptr, 0);
}

Status Client::Upload(UploadType Type, std::span<const uint8_t> Image,
    const UploadOptions &Options, const Progress &OnProgress, RomEntry *Entry)
{
    if (Image.empty()) {
        return Status::BadSize;
    }

    UploadBegin begin{};
    begin.Type = Type;
    begin.Flags = Options.Force ? BeginForce : 0;
    begin.ImageSize = static_cast<uint32_t>(Image.size());
    std::memcpy(begin.Name, Options.Name, NameMax);

    auto status = Transact(Mode::Upload, uint8_t(UploadAction::Begin), &begin, sizeof(begin));
    if (status != Status::Ok) {
        return status;
    }

    std::vector<uint8_t> packet(PayloadMax);
    const uint32_t total = static_cast<uint32_t>(Image.size());
    for (uint32_t offset = 0; offset < total; ) {
        const uint32_t len = std::min<uint32_t>(total - offset, UploadWriteMax);
        std::memcpy(packet.data(), &offset, sizeof(offset));
        std::memcpy(packet.data() + offsetof(UploadWrite, Data), Image.data() + offset, len);

        status = Transact(Mode::Upload, uint8_t(UploadAction::Write), packet.data(),
            static_cast<uint16_t>(offsetof(UploadWrite, Data) + len));
        if (status != Status::Ok) {
            AbortUpload();
            return status;
        }
        offset += len;
        if (OnProgress) {
            OnProgress(offset, total);
        }
    }

    UploadCommit commit{};
    commit.Crc32 = Crc32(Image.data(), Image.size());
    commit.SourceCrc32 = Options.SourceCrc32;
    commit.Version = Options.Version;
    commit.Flags = Options.Flags;

    // Verifies, then erases and programs a slot: seconds, not milliseconds
    status = Transact(Mode::Upload, uint8_t(UploadAction::Commit), &commit, sizeof(commit),
        std::max(TimeoutMs, CommitTimeoutMs));
    if (status == Status::Timeout || status == Status::TransportError) {
        AbortUpload();
    } else if (status == Status::Ok && Entry) {
        TakeReply(*Entry);
    }
    return status;
}

Status Client::Download(DownloadType Type, std::vector<uint8_t> &Out, uint8_t Entry,
    const Progress &OnProgress)
{
    DownloadBegin begin{ Type, Entry };
    auto status = Transact(Mode::Download, uint8_t(DownloadAction::Begin), &begin, sizeof(begin));
    if (status != Status::Ok) {
        return status;
    }
    if (Reply.size() < sizeof(DownloadInfo)) {
        return Status::TransportError;
    }
    DownloadInfo info;
    std::memcpy(&info, Reply.data(), sizeof(info));

    Out.resize(info.Size);
    for (uint32_t offset = 0; offset < info.Size; ) {
        DownloadRead read{ offset, std::min<uint32_t>(info.Size - offset, DownloadReadMax) };
        status = Transact(Mode::Download, uint8_t(DownloadAction::Read), &read, sizeof(read));
        if (status != Status::Ok) {
            break;
        }
        if (Reply.size() != read.Length) {
            status = Status::TransportError;
            break;
        }
        std::memcpy(Out.data() + offset, Reply.data(), read.Length);
        offset += read.Length;
        if (OnProgress) {
            OnProgress(offset, info.Size);
        }
    }

    Transact(Mode::Download, uint8_t(DownloadAction::End), nullptr, 0);
    if (status != Status::Ok) {
        Out.clear();
        return status;
    }
    if (Crc32(Out.data(), Out.size()) != info.Crc32) {
        Out.clear();
        return Status::CrcMismatch;
    }
    return Status::Ok;
}

}
