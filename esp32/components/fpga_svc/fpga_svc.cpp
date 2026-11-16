#include "fpga_svc.hpp"

#include <cstring>

#include "board_pins.hpp"
#include "cfg_bus.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "fpga_link.hpp"
#include "host_link.hpp"

namespace FpgaSvc {
namespace {
const char *Tag = "fpga_svc";

using FpgaLink::Space;

constexpr uint16_t RegIrq = 0x0010;

// Also checked this often without an interrupt: IRQ_ENABLE is lost whenever the FPGA is
// configured again, and the pin floats while it isn't configured
constexpr TickType_t CheckTicks = pdMS_TO_TICKS(100);

constexpr int WaitersMax = 8;

struct Waiter {
    uint16_t Mask;
    TaskHandle_t Task;
};

portMUX_TYPE Mux = portMUX_INITIALIZER_UNLOCKED;
Waiter Waiters[WaitersMax];
int WaiterCount;
uint16_t Enable;
TaskHandle_t Svc;
Stats St;
volatile bool Rearm;

// Level-triggered: the ISR masks itself and the task unmasks it once IRQ_STATUS is clear,
// so a stuck or floating pin costs one wakeup per pass. Not an IRAM interrupt: it calls
// the GPIO driver, which is in flash, and waits out the ESP32's own flash writes (firmware
// updates).
void Isr(void *)
{
    gpio_intr_disable(Pins::FpgaIrq);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(Svc, &woken);
    portYIELD_FROM_ISR(woken);
}

void Dispatch(uint16_t bits)
{
    for (int i = 0; i < 16; i++) {
        if (bits & (1u << i)) {
            St.Bits[i]++;
        }
    }
    Waiter w[WaitersMax];
    int n;
    taskENTER_CRITICAL(&Mux);
    n = WaiterCount;
    memcpy(w, Waiters, sizeof(Waiter) * n);
    taskEXIT_CRITICAL(&Mux);
    for (int i = 0; i < n; i++) {
        if (w[i].Mask & bits) {
            xTaskNotifyGive(w[i].Task);
        }
    }
}

// One pass: IRQ_STATUS and IRQ_ENABLE in one read, the pending bits cleared before anyone
// is told (so an event after that raises the line again), IRQ_ENABLE rewritten if the
// FPGA has lost it. Returns the enabled bits that were pending; -1 if nothing answered.
int Service()
{
    uint8_t r[4];
    if (!FpgaLink::Read(Space::Regs, RegIrq, r, sizeof(r))) {
        return -1;
    }
    if (r[0] == 0xFF && r[1] == 0xFF) {
        return -1;
    }
    const uint16_t status = uint16_t(r[0] | r[1] << 8);
    const uint16_t en = uint16_t(r[2] | r[3] << 8);
    const uint16_t want = Enable;
    if (en != want) {
        const uint8_t w[2] = {uint8_t(want), uint8_t(want >> 8)};
        FpgaLink::Write(Space::Regs, RegIrq + 2, w, 2);
    }
    const uint16_t bits = status & want;
    if (status) {
        FpgaLink::Write(Space::Regs, RegIrq, r, 2);
    }
    if (bits) {
        Dispatch(bits);
    }
    return bits;
}

void Task(void *)
{
    for (;;) {
        bool irq = ulTaskNotifyTake(pdTRUE, CheckTicks) != 0;
        if (Rearm) {
            Rearm = false;
            irq = false;
        }
        int bits = -1;
        if (!CfgBus::Held() && CfgBus::Cdone()) {
            bits = Service();
        }
        if (irq) {
            St.Interrupts++;
            if (bits <= 0) {
                // The line was low with nothing pending: FPGA not configured, or not driving
                // the pin. Leave it masked for a while.
                St.Spurious++;
                vTaskDelay(bits < 0 ? CheckTicks : 1);
            }
        }
        gpio_intr_enable(Pins::FpgaIrq);
    }
}

}

void Start()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ull << Pins::FpgaIrq;
    io.mode = GPIO_MODE_INPUT;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    xTaskCreate(Task, "fpga_svc", 3072, nullptr, 10, &Svc);
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(Tag, "no GPIO ISR service: %s", esp_err_to_name(err));
        return;
    }
    // The line may already be low (the FPGA keeps its IRQs over an ESP32 reset): a level
    // interrupt enabled before its handler is there never clears
    gpio_isr_handler_add(Pins::FpgaIrq, Isr, nullptr);
    gpio_set_intr_type(Pins::FpgaIrq, GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable(Pins::FpgaIrq);
    HostLink::Register(HostLink::Cmd::LinkIrq, [](HostLink::Args &, HostLink::Reply &reply) {
        Stats s = St;
        reply.PutU32(s.Interrupts);
        reply.PutU32(s.Spurious);
        for (uint32_t b : s.Bits) {
            reply.PutU32(b);
        }
    });
}

void Notify(uint16_t mask, TaskHandle_t task)
{
    taskENTER_CRITICAL(&Mux);
    if (WaiterCount < WaitersMax) {
        Waiters[WaiterCount++] = {mask, task};
        Enable |= mask;
    }
    taskEXIT_CRITICAL(&Mux);
    Rearm = true;
    xTaskNotifyGive(Svc);
}

void GetStats(Stats &s)
{
    s = St;
}

}
