#pragma once

#include "driver/gpio.h"

namespace Pins {
inline constexpr gpio_num_t LinkSck = GPIO_NUM_21;
inline constexpr gpio_num_t LinkMosi = GPIO_NUM_22;
inline constexpr gpio_num_t LinkMiso = GPIO_NUM_19;
inline constexpr gpio_num_t LinkCs = GPIO_NUM_20;
inline constexpr gpio_num_t FpgaIrq = GPIO_NUM_34;

inline constexpr gpio_num_t CfgSck = GPIO_NUM_26;
inline constexpr gpio_num_t CfgCs = GPIO_NUM_32;
inline constexpr gpio_num_t CfgMosi = GPIO_NUM_27;
inline constexpr gpio_num_t CfgMiso = GPIO_NUM_36;
inline constexpr gpio_num_t FpgaCreset = GPIO_NUM_25;
inline constexpr gpio_num_t FpgaCdone = GPIO_NUM_35;

// Front panel buttons, open-drain. Not strapping pins, so the ESP32's boot never
// presses them.
inline constexpr gpio_num_t PowerBtn = GPIO_NUM_4;
inline constexpr gpio_num_t EjectBtn = GPIO_NUM_8;

inline constexpr gpio_num_t SmbSda = GPIO_NUM_33;
inline constexpr gpio_num_t SmbScl = GPIO_NUM_13;

}
