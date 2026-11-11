#include "rom_mgr.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "fpga_link.hpp"
#include "nor.hpp"
#include "smbus.hpp"

namespace RomMgr {

using namespace OxProg;

namespace {

const char *Tag = "rom_mgr";

using FpgaLink::Space;

// ------------------------------------------------------------ flash layout
// docs/01-interface.md §6.1: parts up to 4 MB use the small layout
struct Layout {
    uint32_t FlashSize;
    uint32_t CatAddr;           // copy A; copy B follows it
    uint32_t RomBase;
    uint8_t Slots;
};

Layout L;

bool LoadLayout()
{
    uint8_t id[3];
    if (!Nor::ReadId(id) || id[2] < 0x14 || id[2] > 0x19) {
        ESP_LOGE(Tag, "no flash size from JEDEC ID");
        return false;
    }
    L.FlashSize = 1u << id[2];
    const bool small = L.FlashSize <= 4 * 1024 * 1024;
    L.CatAddr = small ? 0x022000 : 0x042000;
    L.RomBase = small ? 0x040000 : 0x100000;
    const uint32_t slots = (L.FlashSize - L.RomBase) / SlotSize;
    L.Slots = uint8_t(slots > 64 ? 64 : slots);
    return true;
}

// ------------------------------------------------------------ catalogue
constexpr uint32_t CatMagic = 0x43524F58;   // "XORC" in flash byte order
constexpr uint16_t CatVersion = 1;
constexpr uint32_t CatSector = 4096;
constexpr uint8_t EntriesMax = 64;

#pragma pack(push, 1)
struct CatEntry {
    Role Kind;
    uint8_t Bank;               // NoBank: unused
    uint8_t Flags;              // RomFlags
    uint8_t Reserved;
    uint32_t Size;
    uint32_t Crc32;
    uint32_t SourceCrc32;
    uint16_t Version;
    uint16_t Reserved2;
    char Name[NameMax];         // NUL-padded, not terminated when full
};

struct Catalogue {
    uint32_t Magic;
    uint16_t Version;
    uint16_t EntrySize;
    uint32_t Sequence;          // wrapping; the newer valid copy wins
    uint8_t Entries;
    uint8_t Reserved[3];
    CatEntry Entry[EntriesMax];
    uint32_t Crc32;             // of everything before it
};
#pragma pack(pop)
static_assert(sizeof(Catalogue) <= CatSector);

Catalogue Cat, Tmp;
int Copy = -1;                  // the copy Cat was read from or last written to
bool Loaded = false;

bool Valid(const CatEntry &e) { return e.Bank != NoBank; }

bool CopyValid(const Catalogue &c)
{
    return c.Magic == CatMagic && c.Version == CatVersion && c.EntrySize == sizeof(CatEntry) &&
           c.Entries == EntriesMax && c.Crc32 == Crc32(&c, offsetof(Catalogue, Crc32));
}

void Empty()
{
    memset(&Cat, 0, sizeof(Cat));
    Cat.Magic = CatMagic;
    Cat.Version = CatVersion;
    Cat.EntrySize = sizeof(CatEntry);
    Cat.Entries = EntriesMax;
    for (CatEntry &e : Cat.Entry) {
        e.Bank = NoBank;
    }
}

void Apply();

bool Load()
{
    if (!LoadLayout()) {
        return false;
    }
    for (int k = 0; k < 2; k++) {
        // A read error is not an empty catalogue: never overwrite one we couldn't read
        if (!Nor::Read(L.CatAddr + k * CatSector, reinterpret_cast<uint8_t *>(&Tmp),
                       sizeof(Tmp))) {
            return false;
        }
        if (CopyValid(Tmp) && (Copy < 0 || int32_t(Tmp.Sequence - Cat.Sequence) > 0)) {
            Cat = Tmp;
            Copy = k;
        }
    }
    if (Copy < 0) {
        ESP_LOGI(Tag, "no catalogue: starting an empty one");
        Empty();
        Copy = 1;               // the first write goes to A
    }
    else {
        ESP_LOGI(Tag, "catalogue %c, sequence %lu", 'A' + Copy, (unsigned long)Cat.Sequence);
    }
    Loaded = true;
    Apply();
    return true;
}

Status Ensure()
{
    return (Loaded || Load()) ? Status::Ok : Status::LayoutInvalid;
}

// Writes the other copy with the next sequence number and verifies it
bool Save()
{
    const int k = Copy ^ 1;
    const uint32_t addr = L.CatAddr + k * CatSector;
    Cat.Sequence++;
    Cat.Crc32 = Crc32(&Cat, offsetof(Catalogue, Crc32));
    uint32_t crc;
    if (!Nor::Erase(addr, CatSector) ||
        !Nor::Write(addr, reinterpret_cast<const uint8_t *>(&Cat), sizeof(Cat)) ||
        !Nor::Crc32(addr, sizeof(Cat), &crc) || crc != Crc32(&Cat, sizeof(Cat))) {
        ESP_LOGE(Tag, "catalogue write to %c failed", 'A' + k);
        return false;
    }
    Copy = k;
    return true;
}

int FindFlag(Role kind, uint8_t flag)
{
    for (int i = 0; i < EntriesMax; i++) {
        if (Valid(Cat.Entry[i]) && Cat.Entry[i].Kind == kind && (Cat.Entry[i].Flags & flag)) {
            return i;
        }
    }
    return NoEntry;
}

int FindBank(uint8_t bank)
{
    for (int i = 0; i < EntriesMax; i++) {
        if (Valid(Cat.Entry[i]) && Cat.Entry[i].Bank == bank) {
            return i;
        }
    }
    return NoEntry;
}

int FreeEntry()
{
    for (int i = 0; i < EntriesMax; i++) {
        if (!Valid(Cat.Entry[i])) {
            return i;
        }
    }
    return NoEntry;
}

uint32_t BankAddr(uint8_t bank) { return L.RomBase + BankSlot(bank) * SlotSize; }

// n slots from s
uint64_t Run(uint32_t s, uint32_t n) { return (n >= 64 ? ~0ull : (1ull << n) - 1) << s; }

uint64_t SlotMask(uint8_t bank)
{
    return bank == NoBank ? 0 : Run(BankSlot(bank), BankSize(bank) / SlotSize);
}

uint32_t Slots(const CatEntry &e) { return e.Size / SlotSize; }

// ------------------------------------------------------------ FPGA bank registers
// docs/01-interface.md §5.2
constexpr uint16_t RegLpcCtrl = 0x0020;
constexpr uint16_t RegPending = 0x0023;
constexpr uint16_t RegDefault = 0x0027;
constexpr uint16_t RegUserBank = 0x003B;   // USER_BANK, SYSTEM_BANK, DIAG_BANK
constexpr uint8_t CtrlArm = 0x08;
constexpr uint8_t StatusArmed = 0x08;

struct Banks {
    uint8_t Ctrl, Status, Active, Pending, Default;
};

bool ReadBanks(Banks &b)
{
    uint8_t r[8];
    if (!FpgaLink::Read(Space::Regs, RegLpcCtrl, r, sizeof(r))) {
        return false;
    }
    b = {r[0], r[1], r[2], r[3], r[7]};
    return true;
}

uint64_t CatSlots()
{
    uint64_t used = 0;
    for (const CatEntry &e : Cat.Entry) {
        if (Valid(e)) {
            used |= SlotMask(e.Bank);
        }
    }
    return used;
}

// What the FPGA serves now, by default and on the armed boot: never written, and the
// entries there never moved
bool PinnedSlots(uint64_t &pinned)
{
    Banks b;
    if (!ReadBanks(b)) {
        return false;
    }
    pinned = SlotMask(b.Active) | SlotMask(b.Default);
    if (b.Status & StatusArmed) {
        pinned |= SlotMask(b.Pending);
    }
    return true;
}

// Slots that must not be written
uint64_t UsedSlots()
{
    uint64_t pinned;
    return PinnedSlots(pinned) ? CatSlots() | pinned : ~0ull;
}

constexpr uint32_t ImageSlotsMax = 4;      // 1 M

// Best fit: of the free runs of n slots aligned to n, the one whose enclosing blocks (up
// to the largest image) are most used, so small images pack together and large aligned
// runs stay open. Slots past the end count as used: a block the end cuts short can't
// take the largest image anyway. Ties go to the lowest slot.
bool Place(uint64_t used, uint32_t n, uint8_t &slot)
{
    const uint64_t full = used | ~Run(0, L.Slots);
    int best = -1;
    int best_score = -1;
    for (uint32_t s = 0; s + n <= L.Slots; s += n) {
        if (full & Run(s, n)) {
            continue;
        }
        int score = 0;
        for (uint32_t m = n * 2; m <= ImageSlotsMax; m *= 2) {
            score += __builtin_popcountll(full & Run(s / m * m, m));
        }
        if (score > best_score) {
            best = int(s);
            best_score = score;
        }
    }
    slot = uint8_t(best);
    return best >= 0;
}

bool Alloc(uint32_t size, uint8_t &bank)
{
    uint8_t s;
    if (!Place(UsedSlots(), size / SlotSize, s)) {
        return false;
    }
    bank = MakeBank(s, size);
    return true;
}

// ------------------------------------------------------------ compaction
// The catalogue entries inside a run, largest first
int EntriesIn(uint64_t run, int (&ids)[EntriesMax])
{
    int k = 0;
    for (int i = 0; i < EntriesMax; i++) {
        if (Valid(Cat.Entry[i]) && (SlotMask(Cat.Entry[i].Bank) & run)) {
            int j = k++;
            for (; j > 0 && Cat.Entry[ids[j - 1]].Size < Cat.Entry[i].Size; j--) {
                ids[j] = ids[j - 1];
            }
            ids[j] = i;
        }
    }
    return k;
}

// The aligned run of n slots cheapest to empty: nothing pinned in it, and every entry in
// it fits elsewhere (placed largest first, as Compact moves them). Moves is the number of
// entries to move.
bool PlanCompact(uint32_t n, uint64_t &run, int &moves)
{
    uint64_t pinned;
    if (!PinnedSlots(pinned)) {
        return false;
    }
    const uint64_t used = CatSlots() | pinned;
    uint32_t best_cost = UINT32_MAX;
    for (uint32_t s = 0; s + n <= L.Slots; s += n) {
        const uint64_t r = Run(s, n);
        if (r & pinned) {
            continue;
        }
        int ids[EntriesMax];
        const int k = EntriesIn(r, ids);
        uint64_t sim = used | r;
        uint32_t cost = 0;
        bool fits = true;
        for (int i = 0; i < k && fits; i++) {
            const uint32_t m = Slots(Cat.Entry[ids[i]]);
            uint8_t to;
            fits = Place(sim, m, to);
            sim |= Run(to, m);
            cost += m;
        }
        if (fits && cost < best_cost) {
            best_cost = cost;
            run = r;
            moves = k;
        }
    }
    return best_cost != UINT32_MAX;
}

uint8_t CopyBuf[4096];

// Copies an entry to dest and verifies it before the catalogue points there; the old
// slots keep their contents until something reuses them
Status Move(int id, uint8_t dest)
{
    CatEntry &e = Cat.Entry[id];
    const uint32_t from = BankAddr(e.Bank);
    const uint32_t to = BankAddr(dest);
    if (!Nor::Erase(to, e.Size)) {
        return Status::FlashError;
    }
    for (uint32_t off = 0; off < e.Size; off += sizeof(CopyBuf)) {
        if (!Nor::Read(from + off, CopyBuf, sizeof(CopyBuf)) ||
            !Nor::Write(to + off, CopyBuf, sizeof(CopyBuf))) {
            return Status::FlashError;
        }
    }
    uint32_t crc;
    if (!Nor::Crc32(to, e.Size, &crc) || crc != e.Crc32) {
        return Status::FlashError;
    }
    const uint8_t old = e.Bank;
    e.Bank = dest;
    if (!Save()) {
        e.Bank = old;
        return Status::FlashError;
    }
    ESP_LOGI(Tag, "entry %d moved from slot %u to %u", id, BankSlot(old), BankSlot(dest));
    return Status::Ok;
}

// The banks the Loader reads (06-boot-flow §4), from the catalogue
void Apply()
{
    auto bank = [](int i) { return i == NoEntry ? NoBank : Cat.Entry[i].Bank; };
    const uint8_t regs[3] = {bank(FindFlag(Role::User, RomUser)),
                             bank(FindFlag(Role::System, RomCurrent)),
                             bank(FindFlag(Role::Diag, RomCurrent))};
    FpgaLink::Write(Space::Regs, RegUserBank, regs, 3);
    const int loader = FindFlag(Role::Loader, RomCurrent);
    if (loader != NoEntry) {
        FpgaLink::Write(Space::Regs, RegDefault, &Cat.Entry[loader].Bank, 1);
    }
}

bool Arm(uint8_t bank)
{
    uint8_t ctrl;
    if (!FpgaLink::Write(Space::Regs, RegPending, &bank, 1) ||
        !FpgaLink::Read(Space::Regs, RegLpcCtrl, &ctrl, 1)) {
        return false;
    }
    ctrl |= CtrlArm;
    return FpgaLink::Write(Space::Regs, RegLpcCtrl, &ctrl, 1);
}

// ------------------------------------------------------------ helpers
template <typename T> bool Take(const uint8_t *in, size_t len, T &v)
{
    // Shorter requests read as zero-filled (older hosts); longer ones are fine (newer)
    memset(&v, 0, sizeof(T));
    memcpy(&v, in, len < sizeof(T) ? len : sizeof(T));
    return len > 0;
}

void ToRomEntry(int i, RomEntry &r)
{
    const CatEntry &e = Cat.Entry[i];
    memset(&r, 0, sizeof(r));
    r.Id = uint8_t(i);
    r.Kind = e.Kind;
    r.Bank = e.Bank;
    r.Flags = e.Flags;
    r.Size = e.Size;
    r.Crc32 = e.Crc32;
    r.SourceCrc32 = e.SourceCrc32;
    r.Version = e.Version;
    memcpy(r.Name, e.Name, NameMax);
}

void SetName(char (&dst)[NameMax], const char *src)
{
    memset(dst, 0, NameMax);
    strncpy(dst, src, NameMax);   // src is NameMax + 1 with a NUL from the host's struct
}

void ClearFlag(Role kind, uint8_t flag)
{
    for (CatEntry &e : Cat.Entry) {
        if (Valid(e) && e.Kind == kind) {
            e.Flags &= ~flag;
        }
    }
}

bool EntryOk(uint8_t id) { return id < EntriesMax && Valid(Cat.Entry[id]); }

// ------------------------------------------------------------ transfers
constexpr int64_t IdleExpireUs = 30 * 1000 * 1000;   // an abandoned upload frees itself
constexpr uint32_t Block = 64 * 1024;

struct UploadState {
    bool Active;
    Role Kind;
    uint8_t Bank;
    uint32_t Size;
    uint16_t Erased;            // 64 K blocks erased so far
    int64_t Last;
    char Name[NameMax];
} Up;

struct DownloadState {
    bool Active;
    uint32_t Addr;
    uint32_t Size;
} Down;

bool UploadLive()
{
    if (Up.Active && esp_timer_get_time() - Up.Last > IdleExpireUs) {
        ESP_LOGW(Tag, "upload abandoned");
        Up.Active = false;
    }
    return Up.Active;
}

bool SizeOk(Role kind, uint32_t size)
{
    const bool std = size == SlotSize || size == 2 * SlotSize || size == 4 * SlotSize;
    return (kind == Role::Loader || kind == Role::Diag) ? size == SlotSize : std;
}

Status UploadBegin(const uint8_t *in, size_t len)
{
    OxProg::UploadBegin b;
    if (!Take(in, len, b)) {
        return Status::BadSize;
    }
    Role kind;
    switch (b.Type) {
    case UploadType::UserRom: kind = Role::User; break;
    case UploadType::LoaderRom: kind = Role::Loader; break;
    case UploadType::SystemRom: kind = Role::System; break;
    case UploadType::DiagRom: kind = Role::Diag; break;
    default: return Status::BadCommand;     // retail tables and the chip's own firmware: later
    }
    if (!SizeOk(kind, b.ImageSize)) {
        return Status::BadSize;
    }
    if (UploadLive()) {
        return Status::ProgrammerBusy;
    }
    const Status st = Ensure();
    if (st != Status::Ok) {
        return st;
    }
    if (FreeEntry() == NoEntry) {
        return Status::NoSpace;
    }
    uint8_t bank;
    if (!Alloc(b.ImageSize, bank)) {
        uint64_t run;
        int moves;
        return PlanCompact(b.ImageSize / SlotSize, run, moves) ? Status::Fragmented
                                                                 : Status::NoSpace;
    }
    Up = {};
    Up.Active = true;
    Up.Kind = kind;
    Up.Bank = bank;
    Up.Size = b.ImageSize;
    Up.Last = esp_timer_get_time();
    SetName(Up.Name, b.Name);
    ESP_LOGI(Tag, "upload %lu bytes to slot %u", (unsigned long)Up.Size, BankSlot(bank));
    return Status::Ok;
}

Status UploadWrite(const uint8_t *in, size_t len)
{
    if (!UploadLive()) {
        return Status::NotStarted;
    }
    if (len <= offsetof(OxProg::UploadWrite, Data)) {
        return Status::BadSize;
    }
    uint32_t off;
    memcpy(&off, in, sizeof(off));
    const uint8_t *data = in + offsetof(OxProg::UploadWrite, Data);
    const uint32_t n = len - offsetof(OxProg::UploadWrite, Data);
    if (off >= Up.Size || n > Up.Size - off) {
        return Status::BadOffset;
    }
    const uint32_t base = BankAddr(Up.Bank);
    for (uint32_t blk = off / Block; blk <= (off + n - 1) / Block; blk++) {
        if (!(Up.Erased & (1u << blk))) {
            if (!Nor::Erase(base + blk * Block, Block)) {
                return Status::FlashError;
            }
            Up.Erased |= 1u << blk;
        }
    }
    if (!Nor::Write(base + off, data, n)) {
        return Status::FlashError;
    }
    Up.Last = esp_timer_get_time();
    return Status::Ok;
}

Status UploadCommit(const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    if (!UploadLive()) {
        return Status::NotStarted;
    }
    OxProg::UploadCommit c;
    Take(in, len, c);
    if (c.Flags & CommitTrial) {
        return Status::BadCommand;              // trial updates: 06-boot-flow §7, later
    }
    if ((c.Flags & CommitUser) && Up.Kind != Role::User) {
        return Status::BadCommand;
    }
    Up.Active = false;
    uint32_t crc;
    // Blocks never written were never erased: the whole range is CRC'd as stored
    if (!Nor::Crc32(BankAddr(Up.Bank), Up.Size, &crc)) {
        return Status::FlashError;
    }
    if (crc != c.Crc32) {
        ESP_LOGW(Tag, "upload CRC %08lX, expected %08lX", (unsigned long)crc,
                 (unsigned long)c.Crc32);
        return Status::CrcMismatch;
    }
    const int id = FreeEntry();
    if (id == NoEntry) {
        return Status::NoSpace;
    }
    if (Up.Kind != Role::User) {
        ClearFlag(Up.Kind, RomCurrent);         // straight to current (no trial)
    }
    if (c.Flags & CommitUser) {
        ClearFlag(Role::User, RomUser);
    }
    CatEntry &e = Cat.Entry[id];
    e = {};
    e.Kind = Up.Kind;
    e.Bank = Up.Bank;
    e.Flags = uint8_t((Up.Kind != Role::User ? RomCurrent : 0) |
                      ((c.Flags & CommitUser) ? RomUser : 0));
    e.Size = Up.Size;
    e.Crc32 = crc;
    e.SourceCrc32 = c.SourceCrc32;
    e.Version = c.Version;
    memcpy(e.Name, Up.Name, NameMax);
    if (!Save()) {
        e.Bank = NoBank;
        return Status::FlashError;
    }
    Apply();
    if ((c.Flags & CommitBoot) && !Arm(e.Bank)) {
        return Status::FlashError;
    }
    ToRomEntry(id, *reinterpret_cast<RomEntry *>(out));
    out_len = sizeof(RomEntry);
    return Status::Ok;
}

Status Compact(const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    RomCompact c;
    if (!Take(in, len, c) || !c.ImageSize || c.ImageSize % SlotSize ||
        c.ImageSize > ImageSlotsMax * SlotSize) {
        return Status::BadSize;
    }
    if (UploadLive()) {
        return Status::ProgrammerBusy;  // its slots aren't in the catalogue yet
    }
    const uint32_t n = c.ImageSize / SlotSize;
    RomCompactStep &step = *reinterpret_cast<RomCompactStep *>(out);
    step = {NoEntry, 0};
    out_len = sizeof(step);
    uint8_t slot;
    if (Place(UsedSlots(), n, slot)) {
        return Status::Ok;
    }
    uint64_t run;
    int moves;
    if (!PlanCompact(n, run, moves)) {
        return Status::NoSpace;
    }
    // The largest entry first, to where the plan put it
    int ids[EntriesMax];
    EntriesIn(run, ids);
    const CatEntry &e = Cat.Entry[ids[0]];
    uint8_t to;
    if (!Place(UsedSlots() | run, Slots(e), to)) {
        return Status::NoSpace;
    }
    const Status st = Move(ids[0], MakeBank(to, e.Size));
    if (st != Status::Ok) {
        return st;
    }
    Apply();
    step = {uint8_t(ids[0]), uint8_t(moves - 1)};
    return Status::Ok;
}

}

// ------------------------------------------------------------ protocol
Status Rom(uint8_t action, const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    const Status st = Ensure();
    if (st != Status::Ok) {
        return st;
    }
    switch (RomAction(action)) {
    case RomAction::List: {
        RomList l;
        Take(in, len, l);
        size_t n = 0;
        for (int i = l.First; i < EntriesMax && n < RomEntriesMax; i++) {
            if (Valid(Cat.Entry[i])) {
                ToRomEntry(i, reinterpret_cast<RomEntry *>(out)[n++]);
            }
        }
        out_len = n * sizeof(RomEntry);
        return Status::Ok;
    }
    case RomAction::Get: {
        RomRef r;
        if (!Take(in, len, r) || !EntryOk(r.Entry)) {
            return Status::NotFound;
        }
        ToRomEntry(r.Entry, *reinterpret_cast<RomEntry *>(out));
        out_len = sizeof(RomEntry);
        return Status::Ok;
    }
    case RomAction::Delete: {
        RomRef r;
        if (!Take(in, len, r) || !EntryOk(r.Entry)) {
            return Status::NotFound;
        }
        CatEntry &e = Cat.Entry[r.Entry];
        if (e.Kind != Role::User && (e.Flags & RomCurrent)) {
            return Status::Protected;
        }
        const CatEntry old = e;
        e.Bank = NoBank;
        if (!Save()) {
            e = old;
            return Status::FlashError;
        }
        Apply();
        return Status::Ok;
    }
    case RomAction::Rename: {
        RomRename r;
        if (!Take(in, len, r) || !EntryOk(r.Entry)) {
            return Status::NotFound;
        }
        SetName(Cat.Entry[r.Entry].Name, r.Name);
        return Save() ? Status::Ok : Status::FlashError;
    }
    case RomAction::SetUser: {
        RomRef r;
        if (!Take(in, len, r)) {
            return Status::BadSize;
        }
        if (r.Entry != NoEntry) {
            if (!EntryOk(r.Entry)) {
                return Status::NotFound;
            }
            if (Cat.Entry[r.Entry].Kind != Role::User) {
                return Status::BadCommand;
            }
        }
        ClearFlag(Role::User, RomUser);
        if (r.Entry != NoEntry) {
            Cat.Entry[r.Entry].Flags |= RomUser;
        }
        if (!Save()) {
            return Status::FlashError;
        }
        Apply();
        return Status::Ok;
    }
    case RomAction::Compact:
        return Compact(in, len, out, out_len);
    default:
        return Status::BadCommand;
    }
}

Status Upload(uint8_t action, const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    switch (UploadAction(action)) {
    case UploadAction::Begin:
        return UploadBegin(in, len);
    case UploadAction::Write:
        return UploadWrite(in, len);
    case UploadAction::Commit:
        return UploadCommit(in, len, out, out_len);
    case UploadAction::Abort:
        Up.Active = false;
        return Status::Ok;
    default:
        return Status::BadCommand;
    }
}

Status Download(uint8_t action, const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    switch (DownloadAction(action)) {
    case DownloadAction::Begin: {
        DownloadBegin b;
        if (!Take(in, len, b)) {
            return Status::BadSize;
        }
        const Status st = Ensure();
        if (st != Status::Ok) {
            return st;
        }
        DownloadInfo &info = *reinterpret_cast<DownloadInfo *>(out);
        if (b.Type == DownloadType::Rom) {
            if (!EntryOk(b.Entry)) {
                return Status::NotFound;
            }
            const CatEntry &e = Cat.Entry[b.Entry];
            Down = {true, BankAddr(e.Bank), e.Size};
            info = {e.Size, e.Crc32};
        }
        else if (b.Type == DownloadType::ActiveRom) {
            Banks bk;
            if (!ReadBanks(bk)) {
                return Status::FlashError;
            }
            Down = {true, BankAddr(bk.Active), BankSize(bk.Active)};
            if (!Nor::Crc32(Down.Addr, Down.Size, &info.Crc32)) {
                Down.Active = false;
                return Status::FlashError;
            }
            info.Size = Down.Size;
        }
        else {
            return Status::NotFound;            // retail tables: later
        }
        out_len = sizeof(DownloadInfo);
        return Status::Ok;
    }
    case DownloadAction::Read: {
        DownloadRead r;
        if (!Take(in, len, r)) {
            return Status::BadSize;
        }
        if (!Down.Active) {
            return Status::NotStarted;
        }
        if (r.Length == 0 || r.Length > DownloadReadMax) {
            return Status::BadSize;
        }
        if (r.Offset >= Down.Size || r.Length > Down.Size - r.Offset) {
            return Status::BadOffset;
        }
        if (!Nor::Read(Down.Addr + r.Offset, out, r.Length)) {
            return Status::FlashError;
        }
        out_len = r.Length;
        return Status::Ok;
    }
    case DownloadAction::End:
        Down.Active = false;
        return Status::Ok;
    default:
        return Status::BadCommand;
    }
}

Status Boot(const BootRequest &req)
{
    const Status st = Ensure();
    if (st != Status::Ok) {
        return st;
    }
    if (!EntryOk(req.Entry)) {
        return Status::NotFound;
    }
    if (!Arm(Cat.Entry[req.Entry].Bank)) {
        return Status::FlashError;
    }
    if ((req.Flags & BootReset) && !Smbus::SmcPower(Smbus::Power::Reset)) {
        return Status::SmbusError;
    }
    return Status::Ok;
}

void FillInfo(Info &info)
{
    if (Ensure() != Status::Ok) {
        return;
    }
    info.UserEntry = uint8_t(FindFlag(Role::User, RomUser));
    info.LoaderEntry = uint8_t(FindFlag(Role::Loader, RomCurrent));
    info.SystemEntry = uint8_t(FindFlag(Role::System, RomCurrent));
    info.DiagEntry = uint8_t(FindFlag(Role::Diag, RomCurrent));
    uint8_t count = 0;
    for (const CatEntry &e : Cat.Entry) {
        count += Valid(e);
    }
    info.EntryCount = count;
    info.SlotCount = L.Slots;
    info.FlashSize = L.FlashSize;
    const uint64_t used = UsedSlots();
    uint8_t free = 0;
    for (int s = 0; s < L.Slots; s++) {
        free += !(used >> s & 1);
    }
    info.FreeSlots = free;
    Banks b;
    if (ReadBanks(b)) {
        info.ActiveEntry = uint8_t(FindBank(b.Active));
        info.ArmedEntry = (b.Status & StatusArmed) ? uint8_t(FindBank(b.Pending)) : NoEntry;
    }
}

}
