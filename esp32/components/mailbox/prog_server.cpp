#include "prog_server.hpp"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "fpga_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "smbus.hpp"

namespace ProgServer {

using namespace OxProg;

namespace {

constexpr size_t HeaderSize = sizeof(Header);

SemaphoreHandle_t Lock = xSemaphoreCreateMutex();   // C++ constructors run after the heap is up

bool HeaderValid(const Header &h)
{
    return h.Magic == Magic && h.Length <= PayloadMax &&
           h.HeaderCrc32 == Crc32(&h, offsetof(Header, HeaderCrc32));
}

// ------------------------------------------------------------ replies
class Out {
public:
    explicit Out(Packet &p) : P(p) {}
    uint8_t *Payload() { return P.Data + HeaderSize; }
    template <typename T> T &As()
    {
        static_assert(sizeof(T) <= PayloadMax);
        memset(Payload(), 0, sizeof(T));
        Len = sizeof(T);
        return *reinterpret_cast<T *>(Payload());
    }
    size_t Len = 0;

private:
    Packet &P;
};

template <typename T> bool Take(const Parser &in, T &v)
{
    // Shorter requests read as zero-filled (older hosts); longer ones are fine (newer)
    memset(&v, 0, sizeof(T));
    const size_t n = in.Head().Length < sizeof(T) ? in.Head().Length : sizeof(T);
    memcpy(&v, in.Payload(), n);
    return n > 0;
}

void RebootTask(void *)
{
    vTaskDelay(pdMS_TO_TICKS(200));             // let the reply go out
    esp_restart();
}

// ------------------------------------------------------------ Control
Status GetInfo(Out &out)
{
    Info &i = out.As<Info>();
    i.Version = Version;
    unsigned a, b, c;                           // a build without x.y.z reports 0.0.0
    if (sscanf(esp_app_get_description()->version, "%u.%u.%u", &a, &b, &c) == 3) {
        i.FirmwareVersion[0] = uint8_t(a);
        i.FirmwareVersion[1] = uint8_t(b);
        i.FirmwareVersion[2] = uint8_t(c);
    }
    uint8_t id[4] = {};
    if (FpgaLink::Read(FpgaLink::Space::Regs, 0x0004, id, 4)) {
        memcpy(i.FpgaVersion, id, 3);
        i.BoardId = id[3];
    }
    uint8_t st = 0, flags = 0;
    FpgaLink::Read(FpgaLink::Space::Regs, 0x0021, &st, 1);
    FpgaLink::Read(FpgaLink::Space::Regs, 0x003A, &flags, 1);
    i.ConsolePowered = st & 0x01;
    i.BootFlags = flags;
    i.ActiveEntry = i.ArmedEntry = i.UserEntry = NoEntry;
    i.LoaderEntry = i.SystemEntry = i.DiagEntry = NoEntry;
    return Status::Ok;
}

Status Control(const Parser &in, Out &out)
{
    switch (ControlAction(in.Head().Cmd.Action())) {
    case ControlAction::GetInfo:
        return GetInfo(out);
    case ControlAction::Reboot:
        xTaskCreate(RebootTask, "prog_reboot", 2048, nullptr, 1, nullptr);
        return Status::Ok;
    case ControlAction::SmbusRead: {
        SmbusTransfer t;
        if (!Take(in, t)) {
            return Status::BadSize;
        }
        SmbusTransfer &r = out.As<SmbusTransfer>();
        r = t;
        return Smbus::ReadByte(t.Address, t.Command, &r.Value) ? Status::Ok : Status::SmbusError;
    }
    case ControlAction::SmbusWrite: {
        SmbusTransfer t;
        if (!Take(in, t)) {
            return Status::BadSize;
        }
        return Smbus::WriteByte(t.Address, t.Command, t.Value) ? Status::Ok : Status::SmbusError;
    }
    default:
        // Boot and ConfirmBoot work on catalogue entries: they come with rom_mgr
        return Status::BadCommand;
    }
}

}

// ------------------------------------------------------------ Parser
bool Parser::MagicPrefix() const
{
    for (size_t i = 0; i < Have && i < 4; i++) {
        if (Buf[i] != uint8_t(Magic >> (8 * i))) {
            return false;
        }
    }
    return true;
}

void Parser::Shift()
{
    // Header rejected: look for the next magic from byte 1 on
    size_t k = 1;
    while (k < Have && Buf[k] != uint8_t(Magic)) {
        k++;
    }
    memmove(Buf, Buf + k, Have - k);
    Have -= k;
}

size_t Parser::Feed(const uint8_t *data, size_t len)
{
    size_t used = 0;
    while (used < len && !Complete()) {
        if (!HdrOk) {
            Buf[Have++] = data[used++];
            if (Have == HeaderSize) {
                memcpy(&Hdr, Buf, HeaderSize);
                if (HeaderValid(Hdr)) {
                    HdrOk = true;
                }
                else {
                    Shift();
                }
            }
            // keep only bytes that can still start a header
            while (Have > 0 && !HdrOk && !MagicPrefix()) {
                Shift();
            }
        }
        else {
            const size_t want = HeaderSize + Hdr.Length - Have;
            const size_t n = (len - used) < want ? (len - used) : want;
            memcpy(Buf + Have, data + used, n);
            Have += n;
            used += n;
        }
    }
    return used;
}

// ------------------------------------------------------------ dispatch
void Handle(const Parser &in, Packet &reply)
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    const Header &h = in.Head();
    Out out(reply);
    Status st;
    if (h.Length && h.PayloadCrc32 != Crc32(in.Payload(), h.Length)) {
        st = Status::CrcMismatch;
    }
    else {
        switch (h.Cmd.Mode()) {
        case Mode::Control:
            st = Control(in, out);
            break;
        default:
            st = Status::BadCommand;
            break;
        }
    }
    if (st != Status::Ok) {
        out.Len = 0;
    }
    Header r = {};
    r.Magic = Magic;
    r.Cmd = h.Cmd;
    r.Result = st;
    r.Length = uint16_t(out.Len);
    r.PayloadCrc32 = out.Len ? Crc32(out.Payload(), out.Len) : 0;
    r.HeaderCrc32 = Crc32(&r, offsetof(Header, HeaderCrc32));
    memcpy(reply.Data, &r, HeaderSize);
    reply.Size = HeaderSize + out.Len;
    xSemaphoreGive(Lock);
}

}
