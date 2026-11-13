#pragma once

// FPGA interrupts (docs/01-interface.md §5.1, docs/03-esp32.md §6): FPGA_IRQ_N wakes one
// task, which reads IRQ_STATUS, clears what it read and notifies the tasks waiting on
// those bits. A notified task then reads whatever state it needs: the bits only say that
// something happened.

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace FpgaSvc {

// IRQ_STATUS bits
namespace Irq {
inline constexpr uint16_t LrstAssert = 1u << 0;
inline constexpr uint16_t LrstDeassert = 1u << 1;
inline constexpr uint16_t BankSwapped = 1u << 2;
inline constexpr uint16_t Usb = 1u << 3;
inline constexpr uint16_t Iotrap = 1u << 4;
inline constexpr uint16_t MbxRx = 1u << 5;
inline constexpr uint16_t ComRx = 1u << 6;
inline constexpr uint16_t Error = 1u << 7;
inline constexpr uint16_t ConsolePower = 1u << 8;     // LCLK started or stopped
inline constexpr uint16_t FlashEmu = 1u << 10;
inline constexpr uint16_t BootCtl = 1u << 11;
}

void Start();

// task gets a notification (xTaskNotifyGive) whenever any of mask is raised
void Notify(uint16_t mask, TaskHandle_t task);

// Interrupts taken and IRQ_STATUS bits seen since Start
struct Stats {
    uint32_t Interrupts;
    uint32_t Spurious;          // the line was low with nothing enabled pending
    uint32_t Bits[16];
};
void GetStats(Stats &s);

}
