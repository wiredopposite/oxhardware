// USB device controller driver for the FPGA's USB device (docs/01-interface.md §5.9).
//
// The FPGA answers every token itself: it ACKs, NAKs and STALLs from slot state, so all
// this driver does is fill IN slots, empty OUT slots and keep the address and endpoint
// maps. Device d's EP0 is IN slot d and OUT slot 16 + d; other endpoints take slots from
// the rest (USBD_DEVICES_MAX..15 in each half) through the endpoint map.
//
// Addresses: a bus reset clears the FPGA's address map, after which device 0 (the hub,
// or the only device) answers at address 0. When the hub resets one of its ports, the
// device behind it takes address 0 over. SET_ADDRESS is deferred in hardware: reading
// the SETUP arms it, and the FPGA maps the new address when the host acknowledges the
// status stage, so no packet goes to the wrong device.
//
// Everything here runs in the one task that calls dcd_fpga_poll() and usbd_task().

#include <cstring>

#include "dcd_fpga.h"
#include "common/usb_util.h"
#include "fpga_link.hpp"
#include "usbd/dcd/dcd.h"
#include "usbd/usbd.h"

static_assert(USBD_DEVICES_MAX <= 8, "the FPGA has 8 devices");

namespace {

using FpgaLink::Space;

// Registers (space 0)
constexpr uint32_t RegCtrl = 0x200;          // bit0 PULLUP, bit1 SIE_EN
constexpr uint32_t RegFrame = 0x202;
constexpr uint32_t RegIrq = 0x204;           // .. 0x20B: IRQ, IRQ_EN, SETUP_DEV, -, IN_DONE, OUT_DONE
constexpr uint32_t RegMapAddr = 0x210;       // MAP_ADDR, MAP_DEV
constexpr uint32_t RegSetaddrDev = 0x212;    // SETADDR_DEV, SETADDR_ADDR
constexpr uint32_t RegSlotCmd = 0x240;

constexpr uint8_t CtrlPullup = 0x01;
constexpr uint8_t CtrlSieEn = 0x02;

constexpr uint8_t IrqBusReset = 0x01;
constexpr uint8_t IrqSuspend = 0x02;
constexpr uint8_t IrqResume = 0x04;
constexpr uint8_t IrqEnable = 0xE7;          // all but SOF

constexpr uint8_t CmdEnable = 0x01;
constexpr uint8_t CmdDisable = 0x02;
constexpr uint8_t CmdStall = 0x04;
constexpr uint8_t CmdUnstall = 0x08;
constexpr uint8_t CmdToggle0 = 0x10;
constexpr uint8_t CmdFree = 0x40;            // IN: disarm, OUT: buffer free

// SPACE 1
constexpr uint32_t BufIn = 0x000;            // + slot * 64
constexpr uint32_t BufOut = 0x400;           // + (slot - 16) * 64
constexpr uint32_t BufSetup = 0x800;         // + dev * 8
constexpr uint32_t OutLen = 0x840;           // + slot - 16
constexpr uint32_t EpMap = 0xC00;            // + dev * 32 + ep * 2 + in
constexpr uint32_t ArmIn = 0xD00;            // + slot

constexpr uint8_t SlotNone = 0xFF;
constexpr uint8_t AddrNone = 0xFF;
constexpr uint16_t MaxPacket = 64;

struct Slot {
    bool used;
    bool busy;              // IN: armed, OUT: holds a packet
    bool stalled;
    uint8_t dev;
    uint8_t epaddr;
    uint16_t size;
};

struct Device {
    uint8_t addr;           // current USB address
    bool reset_pending;     // bus reset seen, its set_address(0) not yet
    uint32_t events;
    uint32_t eps;
    bool setup;             // held back behind events of the previous transfer
    bool hold;              // EP0 events of the previous transfer go to usbd now, after a
                            // SETUP the FPGA already has: keep them off the hardware
};

Slot Slots[32];
Device Devs[USBD_DEVICES_MAX];
uint8_t AddrOwner[128];
dcd_fpga_stats_t St;

bool RegWrite(uint32_t addr, const uint8_t *data, size_t len)
{
    if (FpgaLink::Write(Space::Regs, addr, data, len)) {
        return true;
    }
    St.link_errors++;
    return false;
}

bool RegWrite8(uint32_t addr, uint8_t v)
{
    return RegWrite(addr, &v, 1);
}

bool BufWrite(uint32_t addr, const uint8_t *data, size_t len)
{
    if (FpgaLink::Write(Space::UsbSlots, addr, data, len)) {
        return true;
    }
    St.link_errors++;
    return false;
}

bool BufRead(uint32_t addr, uint8_t *data, size_t len)
{
    if (FpgaLink::Read(Space::UsbSlots, addr, data, len)) {
        return true;
    }
    St.link_errors++;
    return false;
}

bool IsIn(uint8_t epaddr)
{
    return (epaddr & USB_EP_DIR_IN) != 0;
}

// The slot an endpoint has, or SlotNone
uint8_t FindSlot(uint8_t dport, uint8_t epaddr)
{
    if (dport >= USBD_DEVICES_MAX) {
        return SlotNone;
    }
    const uint8_t ep = USB_EP_NUM(epaddr);
    if (ep == 0) {
        return IsIn(epaddr) ? dport : uint8_t(16 + dport);
    }
    const uint8_t base = IsIn(epaddr) ? 0 : 16;
    for (uint8_t s = base + USBD_DEVICES_MAX; s < base + 16; s++) {
        if (Slots[s].used && Slots[s].dev == dport && Slots[s].epaddr == epaddr) {
            return s;
        }
    }
    return SlotNone;
}

Slot *Get(uint8_t dport, uint8_t epaddr)
{
    const uint8_t s = FindSlot(dport, epaddr);
    if (s == SlotNone || !Slots[s].used) {
        return nullptr;
    }
    return &Slots[s];
}

void SlotCmd(uint8_t slot, uint8_t cmd)
{
    RegWrite8(RegSlotCmd + slot, cmd);
}

void MapSet(uint8_t addr, bool valid, uint8_t dport)
{
    const uint8_t w[2] = {addr, uint8_t((valid ? 0x80 : 0x00) | dport)};
    RegWrite(RegMapAddr, w, 2);
}

void Unmap(uint8_t dport)
{
    const uint8_t a = Devs[dport].addr;
    if (a != AddrNone && AddrOwner[a] == dport) {
        MapSet(a, false, 0);
        AddrOwner[a] = AddrNone;
    }
    Devs[dport].addr = AddrNone;
}

void Claim(uint8_t dport, uint8_t addr)
{
    const uint8_t prev = AddrOwner[addr];
    if (prev != AddrNone && prev != dport) {
        Devs[prev].addr = AddrNone;
    }
    MapSet(addr, true, dport);
    AddrOwner[addr] = dport;
    Devs[dport].addr = addr;
}

void ResetState()
{
    for (Slot &s : Slots) {
        s.busy = false;
        s.stalled = false;
    }
    memset(AddrOwner, AddrNone, sizeof(AddrOwner));
    for (Device &d : Devs) {
        d.addr = AddrNone;
        d.reset_pending = true;
        d.events = 0;
        d.eps = 0;
        d.setup = false;
        d.hold = false;
    }
}

bool Held(uint8_t dport, uint8_t epaddr)
{
    return USB_EP_NUM(epaddr) == 0 && dport < USBD_DEVICES_MAX && Devs[dport].hold;
}

uint32_t EpBit(uint8_t epaddr)
{
    return 1u << ((USB_EP_NUM(epaddr) * 2) + (IsIn(epaddr) ? 0 : 1));
}

// A slot's transfer finished: queue it for its device
void Complete(uint8_t slot)
{
    Slot &s = Slots[slot];
    if (!s.used) {
        return;
    }
    if (IsIn(s.epaddr)) {
        s.busy = false;
    } else {
        s.busy = true;
    }
    Devs[s.dev].events |= USBD_EVENT_EP_CMPLT;
    Devs[s.dev].eps |= EpBit(s.epaddr);
}

// ---- dcd_driver_t

bool Init()
{
    memset(Slots, 0, sizeof(Slots));
    ResetState();
    St = {};
    static const uint8_t zero[256] = {};
    const uint8_t clear[8] = {0x0F, IrqEnable, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    return RegWrite8(RegCtrl, CtrlSieEn) && BufWrite(EpMap, zero, sizeof(zero)) &&
           RegWrite(RegIrq, clear, sizeof(clear));
}

void Deinit()
{
    RegWrite8(RegCtrl, 0);
}

void Connect()
{
    RegWrite8(RegCtrl, CtrlSieEn | CtrlPullup);
}

void SetAddress(uint8_t dport, uint8_t daddr)
{
    if (dport >= USBD_DEVICES_MAX || daddr > 127) {
        return;
    }
    Device &d = Devs[dport];
    if (daddr == 0 && d.reset_pending) {
        // usbd resets every device after a bus reset; only device 0 answers at 0 then
        d.reset_pending = false;
        if (dport != 0) {
            return;
        }
    }
    if (d.addr == daddr && AddrOwner[daddr] == dport) {
        return;
    }
    Unmap(dport);
    Claim(dport, daddr);
}

bool EpOpen(uint8_t dport, uint8_t epaddr, uint8_t epattr, uint16_t epsize)
{
    (void)epattr;
    if (dport >= USBD_DEVICES_MAX || epsize > MaxPacket || USB_EP_NUM(epaddr) > 15) {
        return false;
    }
    uint8_t slot = FindSlot(dport, epaddr);
    if (slot == SlotNone) {
        const uint8_t base = IsIn(epaddr) ? 0 : 16;
        for (uint8_t s = base + USBD_DEVICES_MAX; s < base + 16; s++) {
            if (!Slots[s].used) {
                slot = s;
                break;
            }
        }
        if (slot == SlotNone) {
            return false;
        }
    }
    Slots[slot] = {true, false, false, dport, epaddr, epsize};
    if (USB_EP_NUM(epaddr) != 0) {
        const uint8_t m = uint8_t(0x80 | (slot & 15));
        BufWrite(EpMap + dport * 32 + USB_EP_NUM(epaddr) * 2 + (IsIn(epaddr) ? 1 : 0), &m, 1);
        SlotCmd(slot, CmdEnable | CmdUnstall | CmdToggle0 | CmdFree);
    } else {
        SlotCmd(slot, CmdEnable | CmdUnstall | CmdFree);
    }
    return true;
}

void EpClose(uint8_t dport, uint8_t epaddr)
{
    const uint8_t slot = FindSlot(dport, epaddr);
    if (slot == SlotNone || !Slots[slot].used) {
        return;
    }
    if (USB_EP_NUM(epaddr) != 0) {
        const uint8_t m = 0;
        BufWrite(EpMap + dport * 32 + USB_EP_NUM(epaddr) * 2 + (IsIn(epaddr) ? 1 : 0), &m, 1);
    }
    SlotCmd(slot, CmdDisable | CmdUnstall | CmdFree);
    Slots[slot] = {};
}

void EpXferAbort(uint8_t dport, uint8_t epaddr)
{
    const uint8_t slot = FindSlot(dport, epaddr);
    if (slot == SlotNone || !Slots[slot].used) {
        return;
    }
    if (!Held(dport, epaddr)) {
        SlotCmd(slot, CmdFree);
    }
    Slots[slot].busy = false;
}

int32_t EpReadSetup(uint8_t dport, uint8_t *buffer)
{
    if (dport >= USBD_DEVICES_MAX || !BufRead(BufSetup + dport * 8, buffer, 8)) {
        return -1;
    }
    if (buffer[0] == 0x00 && buffer[1] == USB_REQ_STD_SET_ADDRESS) {
        // The FPGA maps the new address when the host acknowledges the status stage
        const uint8_t w[2] = {dport, uint8_t(buffer[2] & 0x7F)};
        RegWrite(RegSetaddrDev, w, 2);
    }
    return 8;
}

int32_t EpRead(uint8_t dport, uint8_t epaddr, uint8_t *buffer, uint16_t len)
{
    const uint8_t slot = FindSlot(dport, epaddr);
    if (slot == SlotNone || !Slots[slot].used || !Slots[slot].busy || IsIn(epaddr)) {
        return -1;
    }
    if (Held(dport, epaddr)) {
        Slots[slot].busy = false;           // the previous status stage, zero length
        return 0;
    }
    uint8_t n = 0;
    if (!BufRead(OutLen + slot - 16, &n, 1)) {
        return -1;
    }
    if (n > MaxPacket) {
        n = MaxPacket;
    }
    if (n > len) {
        n = uint8_t(len);
    }
    if (n > 0 && (buffer == nullptr || !BufRead(BufOut + (slot - 16) * 64, buffer, n))) {
        return -1;
    }
    SlotCmd(slot, CmdFree);
    Slots[slot].busy = false;
    return n;
}

int32_t EpWrite(uint8_t dport, uint8_t epaddr, const uint8_t *buffer, uint16_t len)
{
    const uint8_t slot = FindSlot(dport, epaddr);
    if (slot == SlotNone || !Slots[slot].used || Slots[slot].busy || !IsIn(epaddr)) {
        return -1;
    }
    if (Held(dport, epaddr)) {
        return 0;                           // the previous transfer is over
    }
    uint16_t n = len < Slots[slot].size ? len : Slots[slot].size;
    if (n > 0 && !BufWrite(BufIn + slot * 64, buffer, n)) {
        return -1;
    }
    const uint8_t arm = uint8_t(n);
    if (!BufWrite(ArmIn + slot, &arm, 1)) {
        return -1;
    }
    Slots[slot].busy = true;
    return n;
}

void EpSetStall(uint8_t dport, uint8_t epaddr, bool stall)
{
    Slot *s = Get(dport, epaddr);
    if (s == nullptr) {
        return;
    }
    if (Held(dport, epaddr)) {
        return;
    }
    const uint8_t slot = FindSlot(dport, epaddr);
    SlotCmd(slot, stall ? (CmdStall | CmdFree) : (CmdUnstall | CmdToggle0));
    s->stalled = stall;
    if (stall) {
        s->busy = false;
    }
}

bool EpStalled(uint8_t dport, uint8_t epaddr)
{
    const Slot *s = Get(dport, epaddr);
    return s != nullptr && s->stalled;
}

bool EpReady(uint8_t dport, uint8_t epaddr)
{
    const Slot *s = Get(dport, epaddr);
    if (s == nullptr) {
        return false;
    }
    return IsIn(epaddr) ? !s->busy : s->busy;
}

uint16_t GetFrame()
{
    uint8_t r[2] = {};
    FpgaLink::Read(Space::Regs, RegFrame, r, 2);
    return uint16_t(r[0] | (r[1] & 0x07) << 8);
}

bool Task(uint8_t dport, uint32_t *event_mask, uint32_t *ep_mask)
{
    if (dport >= USBD_DEVICES_MAX) {
        return false;
    }
    Device &d = Devs[dport];
    if (d.setup && d.events == 0) {
        d.events = USBD_EVENT_SETUP;
        d.setup = false;
        d.hold = false;
        Slots[dport].busy = false;
        Slots[16 + dport].busy = false;
        Slots[dport].stalled = Slots[16 + dport].stalled = false;
    }
    *event_mask = d.events;
    *ep_mask = d.eps;
    d.events = 0;
    d.eps = 0;
    return *event_mask != 0;
}

}

extern "C" {

const dcd_driver_t DCD_DRIVER_FPGA = {
    .init = Init,
    .deinit = Deinit,
    .connect = Connect,
    .set_address = SetAddress,
    .ep_open = EpOpen,
    .ep_close = EpClose,
    .ep_xfer_abort = EpXferAbort,
    .ep_read_setup = EpReadSetup,
    .ep_read = EpRead,
    .ep_write = EpWrite,
    .ep_set_stall = EpSetStall,
    .ep_stalled = EpStalled,
    .ep_ready = EpReady,
    .get_frame = GetFrame,
    .task = Task,
};

bool dcd_fpga_poll(void)
{
    uint8_t r[8];
    if (!FpgaLink::Read(Space::Regs, RegIrq, r, sizeof(r))) {
        St.link_errors++;
        return false;
    }
    const uint8_t irq = r[0] & (IrqBusReset | IrqSuspend | IrqResume);   // not SOF
    const uint8_t setup = r[2];
    const uint16_t in_done = uint16_t(r[4] | r[5] << 8);
    const uint16_t out_done = uint16_t(r[6] | r[7] << 8);
    if (r[1] != IrqEnable) {
        RegWrite8(RegIrq + 1, IrqEnable);       // the FPGA was configured again
    }
    if (irq == 0 && setup == 0 && in_done == 0 && out_done == 0) {
        return false;
    }
    // Clear exactly what was read; anything newer raises the interrupt again
    const uint8_t w[8] = {irq, IrqEnable, setup, 0, r[4], r[5], r[6], r[7]};
    RegWrite(RegIrq, w, sizeof(w));
    St.polls++;

    if (irq & IrqBusReset) {
        // Slot state and the address map are gone; completions from before mean nothing
        St.bus_resets++;
        ResetState();
        for (uint8_t i = 0; i < USBD_DEVICES_MAX; i++) {
            Devs[i].events |= USBD_EVENT_RESET;
        }
        Claim(0, 0);
        Devs[0].reset_pending = true;
    } else {
        for (uint8_t k = 0; k < 16; k++) {
            if (in_done & (1u << k)) {
                St.in_done++;
                Complete(k);
            }
            if (out_done & (1u << k)) {
                St.out_done++;
                Complete(uint8_t(16 + k));
            }
        }
    }
    for (uint8_t i = 0; i < USBD_DEVICES_MAX; i++) {
        if (!(setup & (1u << i))) {
            continue;
        }
        St.setups++;
        Device &d = Devs[i];
        // The SETUP reset both EP0 slots in hardware. An EP0 IN completion read with it
        // came before it (nothing was armed after it), and so did a zero-length EP0 OUT
        // (the previous status stage); a data-stage OUT after it has data.
        const uint32_t in0 = EpBit(0 | USB_EP_DIR_IN), out0 = EpBit(0 | USB_EP_DIR_OUT);
        uint8_t n = 0;
        if (d.eps & out0) {
            BufRead(OutLen + i, &n, 1);
        }
        if (n != 0) {
            // SETUP, then its data stage; a stale IN completion is dropped
            d.eps &= ~in0;
            d.events |= USBD_EVENT_SETUP;
            Slots[i].busy = false;
            Slots[i].stalled = Slots[16 + i].stalled = false;
        } else if (d.eps & (in0 | out0)) {
            // the previous transfer's completions now, the SETUP on the next round
            d.setup = true;
            d.hold = true;
        } else {
            d.events |= USBD_EVENT_SETUP;
            Slots[i].busy = Slots[16 + i].busy = false;
            Slots[i].stalled = Slots[16 + i].stalled = false;
        }
    }
    if (irq & IrqSuspend) {
        St.suspends++;
        for (Device &d : Devs) {
            d.events |= USBD_EVENT_SUSPEND;
        }
    }
    if (irq & IrqResume) {
        for (Device &d : Devs) {
            d.events |= USBD_EVENT_RESUME;
        }
    }
    return true;
}

bool dcd_fpga_pending(void)
{
    for (const Device &d : Devs) {
        if (d.events || d.setup) {
            return true;
        }
    }
    return false;
}

void dcd_fpga_get_stats(dcd_fpga_stats_t *stats)
{
    *stats = St;
    for (uint8_t i = 0; i < 8; i++) {
        stats->addr[i] = i < USBD_DEVICES_MAX ? Devs[i].addr : AddrNone;
    }
}

}
