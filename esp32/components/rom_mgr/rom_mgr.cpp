#include "rom_mgr.hpp"

#include <cstdio>
#include <cstring>

#include "cfg_bus.hpp"
#include "chip_update.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "fpga_link.hpp"
#include "fpga_svc.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nor.hpp"
#include "smbus.hpp"

namespace RomMgr {
using namespace OxProg;

namespace {
const char *Tag = "rom_mgr";

// The protocol server and the trial watcher both change the catalogue
SemaphoreHandle_t Lock;

struct Guard {
    Guard() { xSemaphoreTakeRecursive(Lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGiveRecursive(Lock); }
};

using FpgaLink::Space;

// docs/dev/link.md#flash-layout: parts up to 4MB use the small layout
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

constexpr uint32_t CatMagic = 0x43524F58;   // "XORC" in flash byte order
constexpr uint16_t CatVersion = 1;
constexpr uint32_t CatSector = 4096;
constexpr uint8_t EntriesMax = 64;

#pragma pack(push, 1)
struct CatEntry {
    Role Kind;
    uint8_t Bank;
    uint8_t Flags;
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
    uint32_t Crc32;
};
#pragma pack(pop)
static_assert(sizeof(Catalogue) <= CatSector);

Catalogue Cat, Tmp;
int Copy = -1;
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
void TrialResume();

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
    TrialResume();
    return true;
}

Status Ensure()
{
    return (Loaded || Load()) ? Status::Ok : Status::LayoutInvalid;
}

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

// The tables the System BIOS is built from, kept for the console after the catalogue: a
// header sector written last, then the tables. There's one copy, so an upload cut off
// leaves none and the host uploads them again.
constexpr uint32_t TablesMagic = 0x4C425458;    // "XTBL" in flash byte order
constexpr uint32_t TablesArea = 32 * 1024;
constexpr uint32_t TablesMax = TablesArea - CatSector;

#pragma pack(push, 1)
struct TablesHeader {
    uint32_t Magic;
    uint32_t Size;
    uint32_t Crc32;
    uint32_t SourceCrc32;
    uint32_t Check;
};
#pragma pack(pop)

uint32_t TablesAddr() { return L.CatAddr + 2 * CatSector; }
uint32_t TablesData() { return TablesAddr() + CatSector; }

bool ReadTables(TablesHeader &h)
{
    return Nor::Read(TablesAddr(), reinterpret_cast<uint8_t *>(&h), sizeof(h)) &&
           h.Magic == TablesMagic &&
           h.Check == (~TablesMagic ^ h.Size ^ h.Crc32 ^ h.SourceCrc32) && h.Size != 0 &&
           h.Size <= TablesMax;
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

uint64_t Run(uint32_t s, uint32_t n) { return (n >= 64 ? ~0ull : (1ull << n) - 1) << s; }

uint64_t SlotMask(uint8_t bank)
{
    return bank == NoBank ? 0 : Run(BankSlot(bank), BankSize(bank) / SlotSize);
}

uint32_t Slots(const CatEntry &e) { return e.Size / SlotSize; }

constexpr uint16_t RegLpcCtrl = 0x0020;
constexpr uint16_t RegPending = 0x0023;
constexpr uint16_t RegDefault = 0x0027;
constexpr uint16_t RegBootFlags = 0x003A;
constexpr uint16_t RegUserBank = 0x003B;
constexpr uint8_t CtrlArm = 0x08;
constexpr uint8_t StatusArmed = 0x08;
constexpr uint8_t StatusLreset = 0x01;
constexpr uint8_t StatusLclk = 0x02;

// BOOT_FLAGS writes set TRIAL_PENDING from bit2 and LAST_FAILED from bit3; bit6 clears
// TRIAL_CONFIRMED.
constexpr uint8_t FlagArmedBoot = 0x02;
constexpr uint8_t FlagTrial = 0x04;
constexpr uint8_t FlagLastFailed = 0x08;
constexpr uint8_t FlagTrialPending = 0x20;
constexpr uint8_t FlagTrialConfirmed = 0x40;

struct Banks {
    uint8_t Ctrl, Status, Active, Pending, Resets, Default;
};

bool ReadBanks(Banks &b)
{
    uint8_t r[8];
    if (!FpgaLink::Read(Space::Regs, RegLpcCtrl, r, sizeof(r))) {
        return false;
    }
    b = {r[0], r[1], r[2], r[3], r[4], r[7]};
    return true;
}

// Bit 7 always reads 0: 0xFF is an FPGA that isn't there (held in reset, unconfigured)
bool ReadFlags(uint8_t &f)
{
    return FpgaLink::Read(Space::Regs, RegBootFlags, &f, 1) && f != 0xFF;
}

bool WriteFlags(bool trial, bool set_failed = false)
{
    uint8_t f;
    if (!ReadFlags(f)) {
        return false;
    }
    const uint8_t w = uint8_t((set_failed ? FlagLastFailed : (f & FlagLastFailed)) |
                              (trial ? FlagTrial : 0) | FlagTrialConfirmed);
    return FpgaLink::Write(Space::Regs, RegBootFlags, &w, 1);
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

uint64_t UsedSlots()
{
    uint64_t pinned;
    return PinnedSlots(pinned) ? CatSlots() | pinned : ~0ull;
}

constexpr uint32_t ImageSlotsMax = 4;

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

void Apply()
{
    auto bank = [](int i) { return i == NoEntry ? NoBank : Cat.Entry[i].Bank; };
    const uint8_t regs[3] = {bank(FindFlag(Role::User, RomUser)),
                             bank(FindFlag(Role::System, RomCurrent)),
                             bank(FindFlag(Role::Diag, RomCurrent))};
    FpgaLink::Write(Space::Regs, RegUserBank, regs, 3);
    // Without a Loader, System: never the FPGA's own slot 0, which may hold no entry
    int dflt = FindFlag(Role::Loader, RomCurrent);
    if (dflt == NoEntry) {
        dflt = FindFlag(Role::System, RomCurrent);
    }
    if (dflt != NoEntry) {
        FpgaLink::Write(Space::Regs, RegDefault, &Cat.Entry[dflt].Bank, 1);
    }
}

void TrialCancel();
void ClearFlag(Role kind, uint8_t flag);

bool Arm(uint8_t bank, bool trial = false)
{
    if (!trial) {
        TrialCancel();          // the new arm replaces an armed trial
    }
    uint8_t ctrl;
    if (!FpgaLink::Write(Space::Regs, RegPending, &bank, 1) || !WriteFlags(trial) ||
        !FpgaLink::Read(Space::Regs, RegLpcCtrl, &ctrl, 1)) {
        return false;
    }
    ctrl |= CtrlArm;
    return FpgaLink::Write(Space::Regs, RegLpcCtrl, &ctrl, 1);
}

// A new Loader or System is armed with TRIAL and has to confirm within TrialConfirmUs of
// its boot starting, before any further reset. A Loader confirms on the boot control port
// (TRIAL_CONFIRMED outlives the reset it then does), a System with ConfirmBoot.
constexpr int64_t TrialConfirmUs = 10 * 1000 * 1000;

enum class Phase : uint8_t { None, Armed, Running };

struct TrialState {
    Phase Now;
    int Entry;
    uint8_t Bank;
    uint8_t Resets;
    int64_t Start;
} Tr;

TaskHandle_t Watcher;

void TrialArm(int id)
{
    Tr = {Phase::Armed, id, Cat.Entry[id].Bank, 0, 0};
    ESP_LOGI(Tag, "entry %d armed on trial", id);
    xTaskNotifyGive(Watcher);
}

// A trial that will never run, or that the console hasn't been seen running (the ESP32
// restarted with it swapped in and the console off): it stays an entry of its role, not
// current
void TrialCancel()
{
    if (Tr.Now == Phase::None || (Tr.Now == Phase::Running && Tr.Start)) {
        return;
    }
    ESP_LOGI(Tag, "trial of entry %d cancelled", Tr.Entry);
    Cat.Entry[Tr.Entry].Flags &= ~RomTrial;
    Tr.Now = Phase::None;
    Save();
}

// Confirmed in the boot that's running: the FPGA's TRIAL stays set until the next reset, and
// that boot is watched like any other booted one
uint8_t PassedBank = NoBank;
void BootStep();

void TrialPass()
{
    PassedBank = Tr.Bank;
    CatEntry &e = Cat.Entry[Tr.Entry];
    ClearFlag(e.Kind, RomCurrent);
    e.Flags = uint8_t((e.Flags & ~(RomTrial | RomFailedBoot)) | RomCurrent);
    Tr.Now = Phase::None;
    if (!Save()) {
        ESP_LOGE(Tag, "trial of entry %d confirmed but not saved", Tr.Entry);
        return;
    }
    Apply();
    WriteFlags(false);
    ESP_LOGI(Tag, "entry %d confirmed: now the current %s", Tr.Entry,
             e.Kind == Role::Loader ? "Loader" : "System");
    // Its own resets may come before the next poll
    BootStep();
}

// The failure is recorded, LAST_FAILED tells the Loader, and the console is reset unless it
// already has been: the one-shot arm is gone, so it comes back to the old image
void ResetConsole()
{
    if (!Smbus::SmcPower(Smbus::Power::Reset)) {
        Smbus::SmcPower(Smbus::Power::Cycle);
    }
}

void TrialFail(const char *why, bool reset)
{
    CatEntry &e = Cat.Entry[Tr.Entry];
    e.Flags = uint8_t((e.Flags & ~RomTrial) | RomFailedBoot);
    Tr.Now = Phase::None;
    ESP_LOGW(Tag, "trial of entry %d failed: %s", Tr.Entry, why);
    Save();
    WriteFlags(false, true);
    if (reset) {
        ResetConsole();
    }
}

void TrialStep()
{
    // Nothing to watch while the config bus has the FPGA (and its registers start over)
    if (CfgBus::Held() || !CfgBus::Cdone()) {
        return;
    }
    Banks b;
    uint8_t f;
    if (!ReadBanks(b) || !ReadFlags(f)) {
        return;
    }
    if (Tr.Now == Phase::Armed) {
        if ((f & FlagTrial) && b.Active == Tr.Bank) {
            Tr.Now = Phase::Running;
            Tr.Resets = b.Resets;
            Tr.Start = 0;
        }
        else if (!(b.Ctrl & CtrlArm)) {
            TrialCancel();      // disarmed, or the reset served something else
        }
        return;
    }
    if (f & FlagTrialConfirmed) {
        TrialPass();
        return;
    }
    if (!Tr.Start) {
        // Swapped in while the console was off or in reset: the time starts when it runs
        if (b.Status & StatusLreset) {
            Tr.Start = esp_timer_get_time();
            ESP_LOGI(Tag, "trial boot of entry %d started", Tr.Entry);
        }
        else if (b.Resets == Tr.Resets) {
            return;
        }
    }
    else if (b.Resets != Tr.Resets) {
        TrialFail("reset before confirming", false);
    }
    else if (!(b.Status & StatusLclk)) {
        // Off, it counts no reset
        TrialFail("turned off before confirming", false);
    }
    else if (esp_timer_get_time() - Tr.Start > TrialConfirmUs) {
        TrialFail("not confirmed in time", true);
    }
}

// A booted BIOS keeps its bank across the resets and quick power cycles it does itself, as
// it would from a flash chip: some reset the console early on to find a second hard drive.
// The console goes back to the Loader once it has really been off. A boot has failed when
// the SMC gives up on it: bursts of resets, then a FRAG.
constexpr int64_t BootOffUs = 3 * 1000 * 1000;     // LRESET# held this long: really off
constexpr int64_t BootFragWindowUs = 2 * 1000 * 1000;
constexpr uint8_t BootFragResets = 4;              // the SMC retries in bursts of 4
constexpr int64_t BootUpUs = 5 * 1000 * 1000;

constexpr uint16_t RegActive = 0x0022;

struct BootWatch {
    bool On;
    bool Up;
    uint8_t Bank;
    uint8_t Seen;           // LPC_RESET_COUNT
    uint8_t Recent;         // resets since WindowStart
    int64_t WindowStart;
    int64_t Start;          // when it last came out of reset
    int64_t LowSince;       // 0 while running
} Bw;

// Only the arm itself: a trial or LAST_FAILED is left as it is
bool ReArm(uint8_t bank)
{
    uint8_t ctrl;
    if (!FpgaLink::Write(Space::Regs, RegPending, &bank, 1) ||
        !FpgaLink::Read(Space::Regs, RegLpcCtrl, &ctrl, 1)) {
        return false;
    }
    ctrl |= CtrlArm;
    return FpgaLink::Write(Space::Regs, RegLpcCtrl, &ctrl, 1);
}

// Unless something else has been armed since
void DisarmIf(uint8_t bank)
{
    Banks b;
    if (!ReadBanks(b) || !(b.Ctrl & CtrlArm) || b.Pending != bank) {
        return;
    }
    const uint8_t ctrl = uint8_t(b.Ctrl & ~CtrlArm);
    FpgaLink::Write(Space::Regs, RegLpcCtrl, &ctrl, 1);
}

void BootMark(bool failed)
{
    const int id = FindBank(Bw.Bank);
    if (id == NoEntry) {
        return;
    }
    CatEntry &e = Cat.Entry[id];
    const uint8_t flags = uint8_t(failed ? e.Flags | RomFailedBoot : e.Flags & ~RomFailedBoot);
    if (flags != e.Flags) {
        e.Flags = flags;
        Save();
    }
}

// LAST_FAILED tells the Loader, which the reset lands on
void BootFail()
{
    Bw.On = false;
    ESP_LOGW(Tag, "boot of slot %u failed: the SMC keeps resetting it", BankSlot(Bw.Bank));
    uint8_t f;
    if (ReadFlags(f)) {
        const uint8_t w = uint8_t(FlagLastFailed | ((f & FlagTrialPending) ? FlagTrial : 0));
        FpgaLink::Write(Space::Regs, RegBootFlags, &w, 1);
    }
    BootMark(true);
    DisarmIf(Bw.Bank);
    ResetConsole();
}

// Served from the next power-on, which has no reset edge to pick it up
void BackToLoader(const Banks &b)
{
    DisarmIf(Bw.Bank);
    FpgaLink::Write(Space::Regs, RegActive, &b.Default, 1);
    Bw.On = false;
}

void BootStep()
{
    if (CfgBus::Held()) {
        Bw = {};                // the FPGA starts over: so does what it counted
        return;
    }
    // A single low CDONE read is not trusted: it is only a pass skipped
    if (!CfgBus::Cdone()) {
        return;
    }
    Banks b;
    uint8_t f;
    if (!ReadBanks(b) || !ReadFlags(f)) {
        return;
    }
    if (!(f & FlagTrial)) {
        PassedBank = NoBank;
    }
    // A trial is judged on its own
    if (Tr.Now != Phase::None || ((f & FlagTrial) && b.Active != PassedBank)) {
        Bw.On = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    const bool running = b.Status & StatusLreset;
    if (!Bw.On) {
        // Also picks up a boot already running when the ESP32 started
        if (!running || !(f & FlagArmedBoot) || b.Active == b.Default) {
            return;
        }
        Bw = {true, false, b.Active, b.Resets, 0, now, now, 0};
        ESP_LOGI(Tag, "slot %u booted, kept until the console is turned off", BankSlot(Bw.Bank));
    }
    if ((b.Status & StatusArmed) && b.Pending != Bw.Bank) {
        ESP_LOGI(Tag, "slot %u armed: slot %u lets go", BankSlot(b.Pending), BankSlot(Bw.Bank));
        Bw.On = false;
        return;
    }
    if (b.Resets != Bw.Seen) {
        if (now - Bw.WindowStart > BootFragWindowUs) {
            Bw.WindowStart = now;
            Bw.Recent = 0;
        }
        Bw.Recent = uint8_t(Bw.Recent + uint8_t(b.Resets - Bw.Seen));
        Bw.Seen = b.Resets;
        if (Bw.Recent >= BootFragResets) {
            BootFail();
            return;
        }
    }
    if (running) {
        if (Bw.LowSince != 0) {
            Bw.LowSince = 0;
            Bw.Start = now;
        }
        if (!Bw.Up && now - Bw.Start >= BootUpUs) {
            Bw.Up = true;
            BootMark(false);
            ESP_LOGI(Tag, "slot %u is up", BankSlot(Bw.Bank));
        }
    }
    else if (Bw.LowSince == 0) {
        Bw.LowSince = now;
    }
    else if (now - Bw.LowSince >= BootOffUs) {
        ESP_LOGI(Tag, "console off: slot %u lets go", BankSlot(Bw.Bank));
        BackToLoader(b);
        return;
    }
    // A reset takes the arm: it's put back for the next one. Held in reset, it applies at once
    if (!(b.Status & StatusArmed)) {
        ReArm(Bw.Bank);
    }
}

constexpr int WatchPollMs = 50;
constexpr int WatchIdleMs = 1000;

void WatchTask(void *)
{
    for (;;) {
        const bool busy = Tr.Now != Phase::None || Bw.On;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? WatchPollMs : WatchIdleMs));
        Guard g;
        if (Tr.Now != Phase::None) {
            TrialStep();
        }
        BootStep();
    }
}

// After an ESP32 restart: carry on watching a trial the FPGA still has armed or running,
// drop the trial flag from any other
void TrialResume()
{
    Banks b;
    uint8_t f;
    const bool fpga = ReadBanks(b) && ReadFlags(f);
    bool changed = false;
    for (int i = 0; i < EntriesMax; i++) {
        CatEntry &e = Cat.Entry[i];
        if (!Valid(e) || !(e.Flags & RomTrial)) {
            continue;
        }
        if (fpga && Tr.Now == Phase::None && (f & FlagTrial) && b.Active == e.Bank) {
            Tr = {Phase::Running, i, e.Bank, b.Resets, 0};
        }
        else if (fpga && Tr.Now == Phase::None && (b.Ctrl & CtrlArm) && b.Pending == e.Bank &&
                 (f & FlagTrialPending)) {
            Tr = {Phase::Armed, i, e.Bank, 0, 0};
        }
        else {
            e.Flags &= ~RomTrial;
            changed = true;
        }
    }
    if (changed) {
        Save();
    }
}

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

// A role keeps its current image and one spare, which an upload goes into: the spares are
// dropped first. Not the one on trial, nor one the running console is served from.
void DropSpares(Role kind)
{
    if (Tr.Now != Phase::None && Cat.Entry[Tr.Entry].Kind == kind) {
        TrialCancel();
    }
    Banks b;
    const bool on = ReadBanks(b) && (b.Status & StatusLclk);
    bool changed = false;
    for (int i = 0; i < EntriesMax; i++) {
        CatEntry &e = Cat.Entry[i];
        if (!Valid(e) || e.Kind != kind || (e.Flags & RomCurrent) ||
            (Tr.Now != Phase::None && Tr.Entry == i) || (on && b.Active == e.Bank)) {
            continue;
        }
        ESP_LOGI(Tag, "entry %d dropped: the upload replaces it", i);
        e.Bank = NoBank;
        changed = true;
    }
    if (changed) {
        Save();
    }
}

constexpr int64_t IdleExpireUs = 30 * 1000 * 1000;
constexpr uint32_t Block = 64 * 1024;

struct UploadState {
    bool Active;
    bool Tables;
    Role Kind;
    uint8_t Bank;
    uint32_t Size;
    uint16_t Erased;
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
    if (b.Type == UploadType::RetailTables) {
        if (b.ImageSize == 0 || b.ImageSize > TablesMax) {
            return Status::BadSize;
        }
        if (UploadLive() || ChipUpdate::Active()) {
            return Status::ProgrammerBusy;
        }
        const Status st = Ensure();
        if (st != Status::Ok) {
            return st;
        }
        // The old tables are gone from here on: the header is what makes them valid
        if (!Nor::Erase(TablesAddr(), CatSector)) {
            return Status::FlashError;
        }
        Up = {};
        Up.Active = true;
        Up.Tables = true;
        Up.Size = b.ImageSize;
        Up.Last = esp_timer_get_time();
        ESP_LOGI(Tag, "retail tables upload, %lu bytes", (unsigned long)Up.Size);
        return Status::Ok;
    }
    Role kind;
    switch (b.Type) {
    case UploadType::UserRom: kind = Role::User; break;
    case UploadType::LoaderRom: kind = Role::Loader; break;
    case UploadType::SystemRom: kind = Role::System; break;
    case UploadType::DiagRom: kind = Role::Diag; break;
    default: return Status::BadCommand;
    }
    if (!SizeOk(kind, b.ImageSize)) {
        return Status::BadSize;
    }
    if (UploadLive() || ChipUpdate::Active()) {
        return Status::ProgrammerBusy;
    }
    const Status st = Ensure();
    if (st != Status::Ok) {
        return st;
    }
    if (kind != Role::User) {
        DropSpares(kind);
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
    const uint32_t base = Up.Tables ? TablesData() : BankAddr(Up.Bank);
    const uint32_t unit = Up.Tables ? CatSector : Block;
    for (uint32_t blk = off / unit; blk <= (off + n - 1) / unit; blk++) {
        if (!(Up.Erased & (1u << blk))) {
            if (!Nor::Erase(base + blk * unit, unit)) {
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

// The smallest size the image repeats at, by comparing CRCs of its pieces. The FPGA
// mirrors a smaller bank through the 1MB window, so the console reads the same bytes.
bool MirrorSize(uint32_t addr, uint32_t size, uint32_t &out, uint32_t &crc)
{
    out = size;
    for (uint32_t n = SlotSize; n < size; n *= 2) {
        uint32_t first;
        if (!Nor::Crc32(addr, n, &first)) {
            return false;
        }
        bool same = true;
        for (uint32_t off = n; same && off < size; off += n) {
            uint32_t c;
            if (!Nor::Crc32(addr + off, n, &c)) {
                return false;
            }
            same = c == first;
        }
        if (same) {
            out = n;
            crc = first;
            return true;
        }
    }
    return true;
}

Status UploadCommit(const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    if (!UploadLive()) {
        return Status::NotStarted;
    }
    OxProg::UploadCommit c;
    Take(in, len, c);
    if (Up.Tables) {
        Up.Active = false;
        uint32_t crc;
        if (!Nor::Crc32(TablesData(), Up.Size, &crc)) {
            return Status::FlashError;
        }
        if (crc != c.Crc32) {
            ESP_LOGW(Tag, "retail tables CRC %08lX, expected %08lX", (unsigned long)crc,
                     (unsigned long)c.Crc32);
            return Status::CrcMismatch;
        }
        const TablesHeader h = {TablesMagic, Up.Size, crc, c.SourceCrc32,
                                ~TablesMagic ^ Up.Size ^ crc ^ c.SourceCrc32};
        if (!Nor::Write(TablesAddr(), reinterpret_cast<const uint8_t *>(&h), sizeof(h))) {
            return Status::FlashError;
        }
        ESP_LOGI(Tag, "retail tables stored (%lu bytes, source %08lX)", (unsigned long)Up.Size,
                 (unsigned long)c.SourceCrc32);
        return Status::Ok;
    }
    const bool trial = c.Flags & CommitTrial;
    if (trial && Up.Kind != Role::Loader && Up.Kind != Role::System) {
        return Status::BadCommand;
    }
    if (trial) {
        TrialCancel();          // the new trial replaces one that isn't running
        if (Tr.Now != Phase::None) {
            return Status::ProgrammerBusy;
        }
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
    uint32_t size;
    if (!MirrorSize(BankAddr(Up.Bank), Up.Size, size, crc)) {
        return Status::FlashError;
    }
    if (size != Up.Size) {
        ESP_LOGI(Tag, "image repeats every %lu K, stored as that", (unsigned long)(size / 1024));
        Up.Bank = MakeBank(BankSlot(Up.Bank), size);    // the rest of its slots are free again
        Up.Size = size;
    }
    const int id = FreeEntry();
    if (id == NoEntry) {
        return Status::NoSpace;
    }
    const bool current = Up.Kind != Role::User && !trial;
    if (current && Up.Kind == Role::Diag) {
        // One slot: the new one takes over from the old
        const int old = FindFlag(Role::Diag, RomCurrent);
        if (old != NoEntry) {
            Cat.Entry[old].Bank = NoBank;
        }
    }
    else if (current) {
        ClearFlag(Up.Kind, RomCurrent);
    }
    if (c.Flags & CommitUser) {
        ClearFlag(Role::User, RomUser);
    }
    CatEntry &e = Cat.Entry[id];
    e = {};
    e.Kind = Up.Kind;
    e.Bank = Up.Bank;
    e.Flags = uint8_t((current ? RomCurrent : 0) | (trial ? RomTrial : 0) |
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
    if (trial) {
        // A trial is always armed: it is current only once it has booted and confirmed
        if (!Arm(e.Bank, true)) {
            return Status::FlashError;
        }
        TrialArm(id);
    }
    else if ((c.Flags & CommitBoot) && !Arm(e.Bank)) {
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

void Start()
{
    Lock = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(WatchTask, "rom_watch", 4096, nullptr, 5, &Watcher);
    FpgaSvc::Notify(FpgaSvc::Irq::LrstAssert | FpgaSvc::Irq::LrstDeassert |
                        FpgaSvc::Irq::BootCtl,
                    Watcher);
    Guard g;
    Ensure();
    if (Tr.Now != Phase::None) {
        xTaskNotifyGive(Watcher);
    }
}

Status Rom(uint8_t action, const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    Guard g;
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
        if (Tr.Now != Phase::None && Tr.Entry == r.Entry) {
            return Status::ProgrammerBusy;
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
    Guard g;
    const bool chip = ChipUpdate::Active();
    switch (UploadAction(action)) {
    case UploadAction::Begin: {
        OxProg::UploadBegin b;
        if (len >= sizeof(b.Type) && ChipUpdate::Handles(UploadType(in[0]))) {
            if (!Take(in, len, b)) {
                return Status::BadSize;
            }
            return UploadLive() ? Status::ProgrammerBusy : ChipUpdate::Begin(b.Type, b.ImageSize);
        }
        return UploadBegin(in, len);
    }
    case UploadAction::Write:
        if (chip) {
            if (len <= offsetof(OxProg::UploadWrite, Data)) {
                return Status::BadSize;
            }
            uint32_t off;
            memcpy(&off, in, sizeof(off));
            return ChipUpdate::Write(off, in + offsetof(OxProg::UploadWrite, Data),
                                     len - offsetof(OxProg::UploadWrite, Data));
        }
        return UploadWrite(in, len);
    case UploadAction::Commit:
        if (chip) {
            OxProg::UploadCommit c;
            Take(in, len, c);
            return ChipUpdate::Commit(c.Crc32);
        }
        return UploadCommit(in, len, out, out_len);
    case UploadAction::Abort:
        ChipUpdate::Abort();
        Up.Active = false;
        return Status::Ok;
    default:
        return Status::BadCommand;
    }
}

Status Download(uint8_t action, const uint8_t *in, size_t len, uint8_t *out, size_t &out_len)
{
    Guard g;
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
        else if (b.Type == DownloadType::RetailTables) {
            TablesHeader h;
            if (!ReadTables(h)) {
                return Status::NotFound;
            }
            Down = {true, TablesData(), h.Size};
            info = {h.Size, h.Crc32};
        }
        else {
            return Status::BadCommand;
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
    Guard g;
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

Status ConfirmBoot()
{
    Guard g;
    if (Tr.Now == Phase::Armed) {
        TrialStep();            // the boot may have started since the last poll
    }
    if (Tr.Now != Phase::Running) {
        return Status::NotFound;
    }
    Banks b;
    uint8_t f;
    if (!ReadBanks(b) || !ReadFlags(f)) {
        return Status::FlashError;
    }
    if (!(f & FlagTrial) || b.Active != Tr.Bank || b.Resets != Tr.Resets) {
        return Status::NotFound;
    }
    TrialPass();
    return Status::Ok;
}

void FillInfo(Info &info)
{
    Guard g;
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
    TablesHeader h;
    if (ReadTables(h)) {
        info.RetailTablesSize = h.Size;
        info.RetailTablesSourceCrc32 = h.SourceCrc32;
    }
    Banks b;
    if (ReadBanks(b)) {
        info.ActiveEntry = uint8_t(FindBank(b.Active));
        info.ArmedEntry = (b.Status & StatusArmed) ? uint8_t(FindBank(b.Pending)) : NoEntry;
    }
}

}
