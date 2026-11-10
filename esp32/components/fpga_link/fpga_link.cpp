#include "fpga_link.hpp"

#include <cstring>

#include "board_pins.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace FpgaLink {

namespace {

const char *Tag = "fpga_link";

constexpr spi_host_device_t Host = SPI3_HOST;
int ClockHz = 10 * 1000 * 1000;
constexpr size_t Header = 4;            // CMD + 3 address bytes
constexpr size_t MaxDummy = 4;
constexpr size_t BufSize = Header + MaxDummy + MaxXfer;

spi_device_handle_t Dev;
SemaphoreHandle_t Lock;
DMA_ATTR uint8_t TxBuf[BufSize];
DMA_ATTR uint8_t RxBuf[BufSize];

size_t Dummy(Space space)
{
    (void)space;
    return 1;
}

bool Xfer(bool rd, Space space, uint32_t addr, bool noinc, size_t len, uint8_t *sts)
{
    const size_t dummy = rd ? Dummy(space) : 0;
    TxBuf[0] = (rd ? 0x80 : 0x00) | (uint8_t(space) << 4) | (noinc ? 0x08 : 0x00);
    TxBuf[1] = uint8_t(addr >> 16);
    TxBuf[2] = uint8_t(addr >> 8);
    TxBuf[3] = uint8_t(addr);
    if (rd) {
        memset(TxBuf + Header, 0, dummy + len);
    }
    spi_transaction_t t = {};
    t.length = (Header + dummy + len) * 8;
    t.tx_buffer = TxBuf;
    t.rx_buffer = RxBuf;
    const esp_err_t err = spi_device_polling_transmit(Dev, &t);
    if (sts) {
        *sts = RxBuf[0];
    }
    return err == ESP_OK;
}

void BusUp()
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num = Pins::LinkSck;
    bus.mosi_io_num = Pins::LinkMosi;
    bus.miso_io_num = Pins::LinkMiso;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = BufSize;
    ESP_ERROR_CHECK(spi_bus_initialize(Host, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = ClockHz;
    dev.spics_io_num = Pins::LinkCs;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(Host, &dev, &Dev));
}

void BusDown()
{
    spi_bus_remove_device(Dev);
    spi_bus_free(Host);
}

// Mode 0, ~50 kHz
uint8_t BangByte(uint8_t out)
{
    uint8_t in = 0;
    for (int b = 7; b >= 0; b--) {
        gpio_set_level(Pins::LinkMosi, (out >> b) & 1);
        esp_rom_delay_us(10);
        gpio_set_level(Pins::LinkSck, 1);
        in = (in << 1) | gpio_get_level(Pins::LinkMiso);
        esp_rom_delay_us(10);
        gpio_set_level(Pins::LinkSck, 0);
    }
    return in;
}

}

void Init()
{
    Lock = xSemaphoreCreateMutex();
    BusUp();
    ESP_LOGI(Tag, "link up at %d MHz", ClockHz / 1000000);
}

void SetClock(int hz)
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    spi_bus_remove_device(Dev);
    ClockHz = hz;
    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = ClockHz;
    dev.spics_io_num = Pins::LinkCs;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(Host, &dev, &Dev));
    xSemaphoreGive(Lock);
    ESP_LOGI(Tag, "link clock %d kHz", hz / 1000);
}

uint8_t Probe(uint8_t rx[6])
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    BusDown();
    const gpio_num_t outs[] = { Pins::LinkSck, Pins::LinkMosi, Pins::LinkCs };
    for (gpio_num_t p : outs) {
        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_OUTPUT);
    }
    gpio_set_level(Pins::LinkCs, 1);
    gpio_set_level(Pins::LinkSck, 0);
    gpio_set_level(Pins::LinkMosi, 0);
    gpio_reset_pin(Pins::LinkMiso);
    gpio_set_direction(Pins::LinkMiso, GPIO_MODE_INPUT);

    uint8_t levels = 0;
    gpio_set_pull_mode(Pins::LinkMiso, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(100);
    levels |= gpio_get_level(Pins::LinkMiso) << 0;       // CS high, pulled up: 1
    gpio_set_pull_mode(Pins::LinkMiso, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(100);
    levels |= gpio_get_level(Pins::LinkMiso) << 1;       // CS high, pulled down: 0 (Hi-Z)
    gpio_set_pull_mode(Pins::LinkMiso, GPIO_FLOATING);
    gpio_set_level(Pins::LinkCs, 0);
    esp_rom_delay_us(100);
    levels |= gpio_get_level(Pins::LinkMiso) << 2;       // CS low: STS bit 7

    // read 4 bytes of space 0 from address 0: CMD, A, A, A, dummy, ID[4]
    const uint8_t hdr[5] = { 0x80, 0, 0, 0, 0 };
    rx[0] = BangByte(hdr[0]);                             // STS
    for (int i = 1; i < 5; i++) {
        BangByte(hdr[i]);
    }
    for (int i = 1; i < 5; i++) {
        rx[i] = BangByte(0);
    }
    gpio_set_level(Pins::LinkCs, 1);
    esp_rom_delay_us(10);
    rx[5] = gpio_get_level(Pins::LinkMiso);

    for (gpio_num_t p : outs) {
        gpio_reset_pin(p);
    }
    gpio_reset_pin(Pins::LinkMiso);
    BusUp();
    xSemaphoreGive(Lock);
    return levels;
}

bool Read(Space space, uint32_t addr, uint8_t *buf, size_t len, uint8_t *sts, bool noinc)
{
    if (len > MaxXfer) {
        return false;
    }
    xSemaphoreTake(Lock, portMAX_DELAY);
    const bool ok = Xfer(true, space, addr, noinc, len, sts);
    if (ok) {
        memcpy(buf, RxBuf + Header + Dummy(space), len);
    }
    xSemaphoreGive(Lock);
    return ok;
}

bool Write(Space space, uint32_t addr, const uint8_t *data, size_t len, uint8_t *sts,
           bool noinc)
{
    if (len > MaxXfer) {
        return false;
    }
    xSemaphoreTake(Lock, portMAX_DELAY);
    memcpy(TxBuf + Header, data, len);
    const bool ok = Xfer(false, space, addr, noinc, len, sts);
    xSemaphoreGive(Lock);
    return ok;
}

}
