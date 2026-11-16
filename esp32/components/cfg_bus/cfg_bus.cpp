#include "cfg_bus.hpp"

#include <cstdlib>
#include <cstring>

#include "board_pins.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace CfgBus {
namespace {
const char *Tag = "cfg_bus";

constexpr spi_host_device_t Host = SPI2_HOST;
constexpr int ClockHz = 10 * 1000 * 1000;
constexpr size_t MaxXfer = 4096 + 8;

constexpr uint8_t CmdWriteEnable = 0x06;
constexpr uint8_t CmdReadStatus = 0x05;
constexpr uint8_t CmdRead = 0x03;
constexpr uint8_t CmdPageProgram = 0x02;
constexpr uint8_t CmdErase4k = 0x20;
constexpr uint8_t CmdErase64k = 0xD8;
constexpr uint8_t CmdJedecId = 0x9F;
constexpr uint8_t CmdWake = 0xAB;
constexpr uint8_t StatusBusy = 0x01;

spi_device_handle_t Dev;
bool IsHeld = false, IsTaken = false;
ResetHandler BeforeReset, AfterReset;

// Around every CRESET_B low: the FPGA's pins float (with weak pull-ups) until it is
// configured again, so whatever it drives towards the console must let go cleanly first
void ResetStart()
{
    if (const ResetHandler h = BeforeReset) {
        h();
    }
    gpio_set_level(Pins::FpgaCreset, 0);
}

void ResetEnd()
{
    gpio_set_level(Pins::FpgaCreset, 1);
    if (const ResetHandler h = AfterReset) {
        h();
    }
}
// CPU transfers only (no DMA): freeing a DMA-enabled bus on release disturbs the link's
// DMA channel. The hardware buffer holds 64 bytes, so longer transfers are chunked with
// CS# held low.
constexpr size_t Chunk = 64;
uint8_t TxBuf[MaxXfer];
uint8_t RxBuf[MaxXfer];

void PinsFloat()
{
    const gpio_num_t pins[] = { Pins::CfgSck, Pins::CfgCs, Pins::CfgMosi, Pins::CfgMiso };
    for (gpio_num_t p : pins) {
        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_FLOATING);
    }
}

void Select(bool on)
{
    gpio_set_level(Pins::CfgCs, on ? 0 : 1);
}

bool Xfer(size_t len)
{
    Select(true);
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len && err == ESP_OK; off += Chunk) {
        spi_transaction_t t = {};
        const size_t n = len - off < Chunk ? len - off : Chunk;
        t.length = n * 8;
        t.tx_buffer = TxBuf + off;
        t.rx_buffer = RxBuf + off;
        err = spi_device_polling_transmit(Dev, &t);
    }
    Select(false);
    return err == ESP_OK;
}

bool Simple(uint8_t cmd)
{
    TxBuf[0] = cmd;
    return Xfer(1);
}

bool Status(uint8_t *s)
{
    TxBuf[0] = CmdReadStatus;
    TxBuf[1] = 0;
    if (!Xfer(2)) {
        return false;
    }
    *s = RxBuf[1];
    return true;
}

bool WaitReady(uint32_t timeout_ms, bool sleep)
{
    const int64_t end = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    uint8_t s;
    do {
        if (!Status(&s)) {
            return false;
        }
        if (!(s & StatusBusy)) {
            return true;
        }
        if (sleep) {
            vTaskDelay(1);
        }
    } while (esp_timer_get_time() < end);
    ESP_LOGE(Tag, "flash busy timeout");
    return false;
}

void SetAddr(uint8_t cmd, uint32_t addr)
{
    TxBuf[0] = cmd;
    TxBuf[1] = uint8_t(addr >> 16);
    TxBuf[2] = uint8_t(addr >> 8);
    TxBuf[3] = uint8_t(addr);
}

bool Owned()
{
    return IsHeld || IsTaken;
}

bool BusOpen()
{
    gpio_reset_pin(Pins::CfgCs);
    gpio_set_level(Pins::CfgCs, 1);
    gpio_set_direction(Pins::CfgCs, GPIO_MODE_OUTPUT);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = Pins::CfgMosi;
    bus.miso_io_num = Pins::CfgMiso;
    bus.sclk_io_num = Pins::CfgSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = Chunk;
    if (spi_bus_initialize(Host, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        ESP_LOGE(Tag, "spi_bus_initialize failed");
        PinsFloat();
        return false;
    }
    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = ClockHz;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    spi_bus_add_device(Host, &dev, &Dev);

    Simple(CmdWake);
    esp_rom_delay_us(50);
    return true;
}

void BusClose()
{
    spi_bus_remove_device(Dev);
    spi_bus_free(Host);
    PinsFloat();
}

}

void Init()
{
    // CRESET_B: open-drain, released (the UPduino pulls it up)
    gpio_reset_pin(Pins::FpgaCreset);
    gpio_set_level(Pins::FpgaCreset, 1);
    gpio_set_direction(Pins::FpgaCreset, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_reset_pin(Pins::FpgaCdone);
    gpio_set_direction(Pins::FpgaCdone, GPIO_MODE_INPUT);
    PinsFloat();
}

bool Cdone()
{
    return gpio_get_level(Pins::FpgaCdone);
}

void OnReset(ResetHandler before, ResetHandler after)
{
    BeforeReset = before;
    AfterReset = after;
}

bool Held()
{
    return IsHeld;
}

bool Hold()
{
    if (IsHeld) {
        return true;
    }
    if (IsTaken) {
        return false;
    }
    ResetStart();
    esp_rom_delay_us(1000);
    if (!BusOpen()) {
        ResetEnd();
        return false;
    }
    IsHeld = true;
    return true;
}

int64_t Release(uint32_t timeout_ms)
{
    if (IsTaken) {
        return -1;
    }
    if (IsHeld) {
        BusClose();
        IsHeld = false;
    }
    const int64_t start = esp_timer_get_time();
    ResetEnd();
    const int64_t end = start + int64_t(timeout_ms) * 1000;
    while (esp_timer_get_time() < end) {
        if (Cdone()) {
            return esp_timer_get_time() - start;
        }
        vTaskDelay(1);
    }
    return -1;
}

bool Take()
{
    if (IsHeld || IsTaken) {
        return false;
    }
    IsTaken = BusOpen();
    return IsTaken;
}

void Give()
{
    if (!IsTaken) {
        return;
    }
    WaitReady(3000, true);                     // never hand back a busy flash
    BusClose();
    IsTaken = false;
}

uint8_t Probe(uint8_t id[3])
{
    if (IsHeld || IsTaken) {
        return 0xFF;
    }
    ResetStart();
    esp_rom_delay_us(1000);
    const uint8_t idle = uint8_t(gpio_get_level(Pins::CfgCs) |
                                 (gpio_get_level(Pins::CfgSck) << 1) |
                                 (gpio_get_level(Pins::CfgMosi) << 2) |
                                 (gpio_get_level(Pins::CfgMiso) << 3));
    const gpio_num_t outs[] = { Pins::CfgCs, Pins::CfgSck, Pins::CfgMosi };
    for (gpio_num_t p : outs) {
        gpio_set_level(p, p == Pins::CfgCs);
        gpio_set_direction(p, GPIO_MODE_OUTPUT);
    }
    auto byte = [](uint8_t out) {
        uint8_t in = 0;
        for (int b = 7; b >= 0; b--) {
            gpio_set_level(Pins::CfgMosi, (out >> b) & 1);
            esp_rom_delay_us(5);
            gpio_set_level(Pins::CfgSck, 1);
            in = uint8_t((in << 1) | gpio_get_level(Pins::CfgMiso));
            esp_rom_delay_us(5);
            gpio_set_level(Pins::CfgSck, 0);
        }
        return in;
    };
    gpio_set_level(Pins::CfgCs, 0);
    byte(CmdWake);
    gpio_set_level(Pins::CfgCs, 1);
    esp_rom_delay_us(50);
    gpio_set_level(Pins::CfgCs, 0);
    byte(CmdJedecId);
    for (int i = 0; i < 3; i++) {
        id[i] = byte(0);
    }
    gpio_set_level(Pins::CfgCs, 1);
    PinsFloat();
    ResetEnd();
    return idle;
}

bool ReadId(uint8_t id[3])
{
    if (!Owned()) {
        return false;
    }
    memset(TxBuf, 0, 4);
    TxBuf[0] = CmdJedecId;
    if (!Xfer(4)) {
        return false;
    }
    memcpy(id, RxBuf + 1, 3);
    return true;
}

bool Read(uint32_t addr, uint8_t *buf, size_t len)
{
    if (!Owned()) {
        return false;
    }
    while (len) {
        const size_t n = len < 4096 ? len : 4096;
        memset(TxBuf, 0, 4 + n);
        SetAddr(CmdRead, addr);
        if (!Xfer(4 + n)) {
            return false;
        }
        memcpy(buf, RxBuf + 4, n);
        buf += n;
        addr += n;
        len -= n;
    }
    return true;
}

bool Erase(uint32_t addr, uint32_t len)
{
    if (!Owned() || (addr & 0xFFF) || (len & 0xFFF)) {
        return false;
    }
    while (len) {
        const bool big = !(addr & 0xFFFF) && len >= 0x10000;
        if (!Simple(CmdWriteEnable)) {
            return false;
        }
        SetAddr(big ? CmdErase64k : CmdErase4k, addr);
        if (!Xfer(4) || !WaitReady(big ? 3000 : 1000, true)) {
            return false;
        }
        const uint32_t step = big ? 0x10000 : 0x1000;
        addr += step;
        len -= step;
    }
    return true;
}

bool Write(uint32_t addr, const uint8_t *data, size_t len)
{
    if (!Owned()) {
        return false;
    }
    while (len) {
        size_t n = 256 - (addr & 0xFF);
        if (n > len) {
            n = len;
        }
        if (!Simple(CmdWriteEnable)) {
            return false;
        }
        SetAddr(CmdPageProgram, addr);
        memcpy(TxBuf + 4, data, n);
        if (!Xfer(4 + n) || !WaitReady(10, false)) {
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
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
    }
    constexpr uint32_t BufSize = 4096;
    static uint8_t *buf = static_cast<uint8_t *>(malloc(BufSize));     // heap, not static DRAM
    if (buf == nullptr) {
        return false;
    }
    uint32_t c = 0xFFFFFFFFu;
    while (len) {
        const uint32_t n = len < BufSize ? len : BufSize;
        if (!Read(addr, buf, n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            c = table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
        }
        addr += n;
        len -= n;
    }
    *crc = ~c;
    return true;
}

}
