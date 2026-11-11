#include "smbus.hpp"

#include "board_pins.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace Smbus {

namespace {

const char *Tag = "smbus";

constexpr uint32_t ClockHz = 100000;
constexpr int TimeoutMs = 25;
constexpr int Tries = 3;
// What the ESP32 answers as between its own transfers (docs/03-esp32.md §13)
constexpr uint16_t SlaveAddr = 0x60;

SemaphoreHandle_t Lock;
i2c_slave_dev_handle_t Slave;

// Pins back to floating inputs, when neither the master nor the slave has them
void Release()
{
    const gpio_num_t pins[] = {Pins::SmbSda, Pins::SmbScl};
    for (gpio_num_t pin : pins) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
}

// The default state: a slave on I2C1. No clock stretching (the ESP32's slave can't), so
// it never holds SCL; it only ACKs its address for now.
void StartSlave()
{
    i2c_slave_config_t sc = {};
    sc.i2c_port = I2C_NUM_1;
    sc.sda_io_num = Pins::SmbSda;
    sc.scl_io_num = Pins::SmbScl;
    sc.clk_source = I2C_CLK_SRC_DEFAULT;
    sc.send_buf_depth = 32;
    sc.slave_addr = SlaveAddr;
    sc.addr_bit_len = I2C_ADDR_BIT_LEN_7;
    if (i2c_new_slave_device(&sc, &Slave) != ESP_OK) {
        ESP_LOGE(Tag, "slave at 0x%02X failed to start", SlaveAddr);
        Slave = nullptr;
        Release();
    }
}

void StopSlave()
{
    if (Slave) {
        i2c_del_slave_device(Slave);
        Slave = nullptr;
    }
    Release();
}

bool LinesHigh()
{
    return gpio_get_level(Pins::SmbSda) && gpio_get_level(Pins::SmbScl);
}

// The console's MCPX is a master on the same bus: wait until both lines have been high
// for longer than any SCL high or low period, so we don't start inside its transfer.
bool WaitIdle()
{
    for (int ms = 0; ms < TimeoutMs; ms++) {
        int quiet = 0;
        while (quiet < 12 && LinesHigh()) {
            esp_rom_delay_us(5);
            quiet++;
        }
        if (quiet == 12) {
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

esp_err_t Once(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = Pins::SmbSda;
    bc.scl_io_num = Pins::SmbScl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = false;
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bc, &bus);
    if (err != ESP_OK) {
        return err;
    }
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr & 0x7F;
    dc.scl_speed_hz = ClockHz;
    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(bus, &dc, &dev);
    if (err == ESP_OK) {
        err = rx_len ? i2c_master_transmit_receive(dev, tx, tx_len, rx, rx_len, TimeoutMs)
                     : i2c_master_transmit(dev, tx, tx_len, TimeoutMs);
        i2c_master_bus_rm_device(dev);
    }
    i2c_del_master_bus(bus);
    Release();
    return err;
}

bool Xfer(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < Tries; i++) {
        if (WaitIdle()) {
            StopSlave();
            err = Once(addr, tx, tx_len, rx, rx_len);
            StartSlave();
        }
        else {
            err = ESP_ERR_TIMEOUT;
        }
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!LinesHigh()) {
        ESP_LOGW(Tag, "bus low after a transfer (SDA %d SCL %d)", gpio_get_level(Pins::SmbSda),
                 gpio_get_level(Pins::SmbScl));
    }
    xSemaphoreGive(Lock);
    if (err != ESP_OK) {
        ESP_LOGD(Tag, "0x%02X: %s", addr, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

}

void Init()
{
    Lock = xSemaphoreCreateMutex();
    StartSlave();
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
