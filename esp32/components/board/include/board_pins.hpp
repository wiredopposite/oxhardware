#pragma once

// ESP32 pin assignments. Bench setup: ESP32-PICO-DevKitM-2 wired to a UPduino v3.1,
// docs/05-devboard-wiring.md.

#include "driver/gpio.h"

namespace Pins {

// ESP32 <-> FPGA link (VSPI through the GPIO matrix)
inline constexpr gpio_num_t LinkSck = GPIO_NUM_21;
inline constexpr gpio_num_t LinkMosi = GPIO_NUM_22;
inline constexpr gpio_num_t LinkMiso = GPIO_NUM_19;
inline constexpr gpio_num_t LinkCs = GPIO_NUM_20;
inline constexpr gpio_num_t FpgaIrq = GPIO_NUM_34;

// Config bus (FPGA's flash) and FPGA control
inline constexpr gpio_num_t CfgSck = GPIO_NUM_26;
inline constexpr gpio_num_t CfgCs = GPIO_NUM_32;
inline constexpr gpio_num_t CfgMosi = GPIO_NUM_27;
inline constexpr gpio_num_t CfgMiso = GPIO_NUM_36;
inline constexpr gpio_num_t FpgaCreset = GPIO_NUM_25;
inline constexpr gpio_num_t FpgaCdone = GPIO_NUM_35;

// Xbox SMBus (console pull-ups)
inline constexpr gpio_num_t SmbSda = GPIO_NUM_33;
inline constexpr gpio_num_t SmbScl = GPIO_NUM_13;

}
