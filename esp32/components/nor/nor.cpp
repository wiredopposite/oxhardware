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

// docs/01-interface.md §5.5
constexpr uint16_t RegFeatures = 0x0008;
constexpr uint16_t RegFlashCtrl = 0x00C0;     // bit0 REQ, bit1 GRANT
constexpr uint16_t RegFlashMiss = 0x00C1;
constexpr uint16_t FeatureFlashShare = 1u << 9;
constexpr uint8_t CtrlReq = 0x01;
constexpr uint8_t CtrlGrant = 0x02;

// The FPGA grants the flash once the console has gone 1 ms without reading it; a boot
// reads it for about half a second
constexpr int64_t GrantTimeoutUs = 3 * 1000 * 1000;

constexpr uint32_t Sector = 4 * 1024;

SemaphoreHandle_t Lock;
uint32_t ProtectEnd;        // the bitstream area: erase and program refused below it

class Guard {
public:
    Guard() { xSemaphoreTake(Lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(Lock); }
};

bool HasShare()
{
    uint8_t f[2];
    return FpgaLink::Read(Space::Regs, RegFeatures, f, sizeof(f)) && f[0] != 0xFF &&
           ((f[0] | f[1] << 8) & FeatureFlashShare);
}

void Ask(bool want)
{
    const uint8_t v = want ? CtrlReq : 0;
    FpgaLink::Write(Space::Regs, RegFlashCtrl, &v, 1);
}

// The flash for one operation: held (FPGA in reset) as it is, or borrowed from the
// running FPGA for the scope's lifetime
class Access {
public:
    Access()
    {
        if (CfgBus::Held()) {
            ok_ = true;
            return;
        }
        if (!HasShare()) {
            ESP_LOGE(Tag, "FPGA design can't lend the flash");
            return;
        }
        Ask(true);
        const int64_t start = esp_timer_get_time();
        for (;;) {
            uint8_t st;
            if (FpgaLink::Read(Space::Regs, RegFlashCtrl, &st, 1) && (st & CtrlGrant)) {
                break;
            }
            if (esp_timer_get_time() - start > GrantTimeoutUs) {
                ESP_LOGE(Tag, "FPGA didn't grant the flash");
                Ask(false);
                return;
            }
            vTaskDelay(1);
        }
        if (!CfgBus::Take()) {
            Ask(false);
            return;
        }
        borrowed_ = true;
        ok_ = true;
    }
    ~Access()
    {
        if (!borrowed_) {
            return;
        }
        CfgBus::Give();
        Ask(false);
        uint8_t miss;
        if (FpgaLink::Read(Space::Regs, RegFlashMiss, &miss, 1) && miss != 0) {
            ESP_LOGW(Tag, "console read the flash %u times while it was lent (got 0xFF)", miss);
            const uint8_t zero = 0;
            FpgaLink::Write(Space::Regs, RegFlashMiss, &zero, 1);
        }
    }
    bool Ok() const { return ok_; }
    bool Held() const { return ok_ && !borrowed_; }

private:
    bool ok_ = false;
    bool borrowed_ = false;
};

// The bitstream area ends at the catalogue (§6.1): parts up to 4 MB use the small layout
bool Protected(const Access &a, uint32_t addr)
{
    if (a.Held()) {
        return false;           // factory and recovery writes
    }
    if (ProtectEnd == 0) {
        uint8_t id[3];
        if (!CfgBus::ReadId(id) || id[2] < 0x14 || id[2] > 0x19) {
            return true;
        }
        ProtectEnd = (1u << id[2]) <= 4 * 1024 * 1024 ? 0x022000 : 0x042000;
    }
    if (addr < ProtectEnd) {
        ESP_LOGW(Tag, "%06lX is in the bitstream area", (unsigned long)addr);
        return true;
    }
    return false;
}

}

void Init()
{
    Lock = xSemaphoreCreateMutex();
}

bool Ready()
{
    return CfgBus::Held() || HasShare();
}

bool ReadId(uint8_t id[3])
{
    Guard g;
    Access a;
    return a.Ok() && CfgBus::ReadId(id);
}

bool Read(uint32_t addr, uint8_t *buf, size_t len)
{
    Guard g;
    Access a;
    return a.Ok() && CfgBus::Read(addr, buf, len);
}

namespace {

bool DoErase(uint32_t addr, uint32_t len, bool bitstream)
{
    if ((addr | len) & (Sector - 1)) {
        return false;
    }
    Guard g;
    Access a;
    if (!a.Ok() || (!bitstream && Protected(a, addr))) {
        return false;
    }
    if (!CfgBus::Erase(addr, len)) {
        ESP_LOGE(Tag, "erase at %06lX failed", (unsigned long)addr);
        return false;
    }
    return true;
}

bool DoWrite(uint32_t addr, const uint8_t *data, size_t len, bool bitstream)
{
    Guard g;
    Access a;
    if (!a.Ok() || (!bitstream && Protected(a, addr))) {
        return false;
    }
    if (!CfgBus::Write(addr, data, len)) {
        ESP_LOGE(Tag, "program at %06lX failed", (unsigned long)addr);
        return false;
    }
    return true;
}

}

bool Erase(uint32_t addr, uint32_t len)
{
    return DoErase(addr, len, false);
}

bool Write(uint32_t addr, const uint8_t *data, size_t len)
{
    return DoWrite(addr, data, len, false);
}

bool EraseBitstream(uint32_t len)
{
    return len <= BitstreamArea && DoErase(0, len, true);
}

bool WriteBitstream(uint32_t addr, const uint8_t *data, size_t len)
{
    return addr <= BitstreamArea && len <= BitstreamArea - addr &&
           DoWrite(addr, data, len, true);
}

bool Crc32(uint32_t addr, uint32_t len, uint32_t *crc)
{
    if (len == 0 || len > 0xFFFFFF) {
        return false;
    }
    Guard g;
    Access a;
    return a.Ok() && CfgBus::Crc32(addr, len, crc);
}

}
