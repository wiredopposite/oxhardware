#include "smbus.hpp"

#include "board_pins.hpp"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace Smbus {

namespace {

const char *Tag = "smbus";

constexpr uint32_t ClockHz = 100000;
constexpr int TimeoutMs = 25;
constexpr int Tries = 3;

i2c_master_bus_handle_t Bus;

// One device handle per address, created on first use.
i2c_master_dev_handle_t Devs[128];

i2c_master_dev_handle_t Dev(uint8_t addr)
{
    if (!Devs[addr & 0x7F]) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = addr & 0x7F;
        cfg.scl_speed_hz = ClockHz;
        if (i2c_master_bus_add_device(Bus, &cfg, &Devs[addr & 0x7F]) != ESP_OK) {
            return nullptr;
        }
    }
    return Devs[addr & 0x7F];
}

bool Xfer(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    i2c_master_dev_handle_t dev = Dev(addr);
    if (!dev) {
        return false;
    }
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < Tries; i++) {
        err = rx_len ? i2c_master_transmit_receive(dev, tx, tx_len, rx, rx_len, TimeoutMs)
                     : i2c_master_transmit(dev, tx, tx_len, TimeoutMs);
        if (err == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGD(Tag, "0x%02X: %s", addr, esp_err_to_name(err));
    return false;
}

}

void Init()
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = Pins::SmbSda;
    cfg.scl_io_num = Pins::SmbScl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = false;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &Bus));
}

bool ReadByte(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return Xfer(addr, &reg, 1, val, 1);
}

bool ReadWord(uint8_t addr, uint8_t reg, uint16_t *val)
{
    uint8_t b[2];
    if (!Xfer(addr, &reg, 1, b, 2)) {
        return false;
    }
    *val = uint16_t(b[0] | b[1] << 8);
    return true;
}

bool WriteByte(uint8_t addr, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return Xfer(addr, b, 2, nullptr, 0);
}

bool WriteWord(uint8_t addr, uint8_t reg, uint16_t val)
{
    const uint8_t b[3] = { reg, uint8_t(val), uint8_t(val >> 8) };
    return Xfer(addr, b, 3, nullptr, 0);
}

bool SmcPower(Power action)
{
    return WriteByte(Addr::Smc, 0x02, uint8_t(action));
}

}
