#include "nor.hpp"

#include "cfg_bus.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "fpga_link.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace Nor {

namespace {

const char *Tag = "nor";

using FpgaLink::Space;

// Link registers, docs/01-interface.md §5.5
constexpr uint16_t RegStatus = 0x00C0;
constexpr uint16_t RegJedec = 0x00C2;
constexpr uint16_t RegAddr = 0x00C8;      // 3 bytes, then NOR_OP at 0xCB
constexpr uint16_t RegLen = 0x00CC;
constexpr uint16_t RegCrcAddr = 0x00D0;   // 3 bytes address, 3 bytes length, CRC_CTRL
constexpr uint16_t RegCrc = 0x00D8;
constexpr uint32_t BufferPort = 0x03;     // SPACE 3

constexpr uint8_t StInit = 0x01;
constexpr uint8_t StBusy = 0x04;
constexpr uint8_t StRefused = 0x20;

enum class Op : uint8_t {
    Erase4K = 1,
    Erase64K = 2,
    Program = 3,
    Read = 5,
};

constexpr size_t Page = 256;
constexpr uint32_t Block = 64 * 1024;
constexpr uint32_t Sector = 4 * 1024;

// Ops wait for the console to leave the flash alone for 1 ms (a boot reads it for about
// half a second), and BIOS reads suspend erases and programs on top of that.
constexpr int64_t TimeoutUs = 10 * 1000 * 1000;

SemaphoreHandle_t Lock;

class Guard {
public:
    Guard() { xSemaphoreTake(Lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(Lock); }
};

bool Status(uint8_t &st)
{
    return FpgaLink::Read(Space::Regs, RegStatus, &st, 1);
}

// Waits for the engine to go idle; false on a timeout or a refused op
bool Wait()
{
    const int64_t start = esp_timer_get_time();
    for (int polls = 0;; polls++) {
        uint8_t st;
        if (!Status(st)) {
            return false;
        }
        if (!(st & StBusy)) {
            if (st & StRefused) {
                ESP_LOGW(Tag, "op refused");
                return false;
            }
            return true;
        }
        if (esp_timer_get_time() - start > TimeoutUs) {
            ESP_LOGE(Tag, "engine busy for %d s", int(TimeoutUs / 1000000));
            return false;
        }
        if (polls > 32) {
            vTaskDelay(1);                  // an erase, or the console booting
        }
    }
}

bool Start(Op op, uint32_t addr, size_t len)
{
    const uint8_t n = uint8_t(len - 1);
    const uint8_t regs[4] = {uint8_t(addr), uint8_t(addr >> 8), uint8_t(addr >> 16),
                             uint8_t(op)};
    // NOR_LEN first: the op starts as NOR_OP is written
    return FpgaLink::Write(Space::Regs, RegLen, &n, 1) &&
           FpgaLink::Write(Space::Regs, RegAddr, regs, 4) && Wait();
}

bool ReadChunk(uint32_t addr, uint8_t *buf, size_t len)
{
    return Start(Op::Read, addr, len) &&
           FpgaLink::Read(Space::Fifos, BufferPort, buf, len, nullptr, true);
}

bool ProgramChunk(uint32_t addr, const uint8_t *data, size_t len)
{
    return FpgaLink::Write(Space::Fifos, BufferPort, data, len, nullptr, true) &&
           Start(Op::Program, addr, len);
}

}

void Init()
{
    Lock = xSemaphoreCreateMutex();
}

bool Ready()
{
    if (CfgBus::Held()) {
        return true;
    }
    uint8_t st;
    return Status(st) && (st & StInit) && !(st & StBusy);
}

bool ReadId(uint8_t id[3])
{
    Guard g;
    if (CfgBus::Held()) {
        return CfgBus::ReadId(id);
    }
    uint8_t st;
    return Status(st) && (st & StInit) && FpgaLink::Read(Space::Regs, RegJedec, id, 3);
}

bool Read(uint32_t addr, uint8_t *buf, size_t len)
{
    Guard g;
    if (CfgBus::Held()) {
        return CfgBus::Read(addr, buf, len);
    }
    while (len) {
        const size_t n = len < Page ? len : Page;
        if (!ReadChunk(addr, buf, n)) {
            ESP_LOGE(Tag, "read at %06lX failed", (unsigned long)addr);
            return false;
        }
        addr += n;
        buf += n;
        len -= n;
    }
    return true;
}

bool Erase(uint32_t addr, uint32_t len)
{
    if ((addr | len) & (Sector - 1)) {
        return false;
    }
    Guard g;
    if (CfgBus::Held()) {
        return CfgBus::Erase(addr, len);
    }
    while (len) {
        const bool big = !(addr & (Block - 1)) && len >= Block;
        if (!Start(big ? Op::Erase64K : Op::Erase4K, addr, 1)) {
            ESP_LOGE(Tag, "erase at %06lX failed", (unsigned long)addr);
            return false;
        }
        const uint32_t n = big ? Block : Sector;
        addr += n;
        len -= n;
    }
    return true;
}

bool Write(uint32_t addr, const uint8_t *data, size_t len)
{
    Guard g;
    if (CfgBus::Held()) {
        return CfgBus::Write(addr, data, len);
    }
    while (len) {
        const size_t room = Page - (addr & (Page - 1));
        const size_t n = len < room ? len : room;
        if (!ProgramChunk(addr, data, n)) {
            ESP_LOGE(Tag, "program at %06lX failed", (unsigned long)addr);
            return false;
        }
        addr += n;
        data += n;
        len -= n;
    }
    return true;
}

bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc)
{
    if (len == 0 || len > 0xFFFFFF) {
        return false;
    }
    Guard g;
    if (CfgBus::Held()) {
        return CfgBus::Crc32(addr, len, crc);
    }
    const uint8_t regs[7] = {uint8_t(addr), uint8_t(addr >> 8), uint8_t(addr >> 16),
                             uint8_t(len),  uint8_t(len >> 8),  uint8_t(len >> 16), 1};
    uint8_t r[4];
    if (!FpgaLink::Write(Space::Regs, RegCrcAddr, regs, 7) || !Wait() ||
        !FpgaLink::Read(Space::Regs, RegCrc, r, 4)) {
        return false;
    }
    *crc = r[0] | r[1] << 8 | r[2] << 16 | uint32_t(r[3]) << 24;
    return true;
}

}
