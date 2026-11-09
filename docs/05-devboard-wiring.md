# 05 — Dev Board Wiring (UPduino v3.1 + ESP32-PICO-DevKitM-2)

Bench setup for phases 0–1: the UPduino stands in for the FPGA side, the ESP32-PICO-DevKitM-2 (ESP32-PICO-MINI-02, 8 MB flash, 2 MB PSRAM) for the ESP32 side. Final-board pin choices are in [04-hardware.md](04-hardware.md); the signals are defined in [01-interface.md](01-interface.md).

UPduino pins are given by their silkscreen label, which is the FPGA package pin number. Signals that are not FPGA user I/O (flash SPI, `CRESET_N`, `CDONE`) are on the UPduino's J3/J2 headers, listed by header pin; check the silkscreen before wiring.

## 1. Power and ground

| Connection | Notes |
|---|---|
| UPduino GND ↔ ESP32 devkit GND ↔ Xbox GND | One short common ground. Each board is powered from its own USB cable. |
| 3.3 V rails | **Not** connected between boards. |

All signals are 3.3 V on both sides.

## 2. ESP32 ↔ FPGA link

SPI mode 0, ESP32 master. On the ESP32 the link uses the GPIO matrix (the module has no GPIO18/23 — they are wired to its in-package flash), so the link clock is limited to 26.67 MHz.

| Signal | UPduino | ESP32 devkit | Notes |
|---|---|---|---|
| `LINK_SCK` | 44 | IO21 (J2-2) | FPGA pin 44 is a global-buffer input |
| `LINK_MOSI` | 45 | IO22 (J2-3) | |
| `LINK_MISO` | 47 | IO19 (J2-4) | FPGA tri-states it while `LINK_CS_N` is high |
| `LINK_CS_N` | 48 | IO20 (J2-1) | |
| `FPGA_IRQ_N` | 4 | IO34 (J2-13) | ESP32 input-only pin; FPGA drives push-pull |

## 3. Config bus and FPGA control

Lets the ESP32 hold the FPGA in reset and program its flash directly (01-interface §2.2). The UPduino's FT232H is on the same nets; it releases them when `iceprog` is not running, so **never run `iceprog` and an ESP32 flash operation at the same time.** Put 100 Ω in series with each ESP32 output (`CFG_SCK`, `CFG_SS_N`, `CFG_MOSI`, `FPGA_CRESET_N`) to limit current if both ever drive.

| Signal | UPduino | ESP32 devkit | Notes |
|---|---|---|---|
| `CFG_SCK` | J3-2 (FT_SCK, FPGA pin 15) | IO26 (J3-5) | |
| `CFG_SS_N` | J3-1 (FT_SSn, FPGA pin 16) | IO32 (J3-6) | |
| `CFG_MOSI` (ESP32 → flash DI) | J3-3 (FLASH_MOSI, FPGA pin 14) | IO27 (J3-8) | |
| `CFG_MISO` (flash DO → ESP32) | J3-4 (FLASH_MISO, FPGA pin 17) | IO36 / SENSOR_VP (J3-2) | ESP32 input-only pin |
| `FPGA_CRESET_N` | J2-3 (CRESET_N) | IO25 (J3-4) | ESP32 drives open-drain; low = FPGA held in reset |
| `FPGA_CDONE` | J2-4 (CDONE) | IO35 (J2-12) | ESP32 input-only pin |

ESP32 strapping pins (IO0, IO2, IO5, IO12, IO15) are not used, so an unconfigured FPGA's pull-ups can't change how the ESP32 boots.

## 4. Xbox and debug (unchanged)

| Signal | UPduino | Notes |
|---|---|---|
| LCLK | 37 | 33 Ω |
| LRESET# | 32 | 100 Ω |
| LAD0 / LAD1 / LAD2 / LAD3 | 34 / 43 / 36 / 42 | 33 Ω |
| LFRAME# MOSFET gate | 31 (+ 25 / 26 / 27) | 04-hardware §4.2 |
| LFRAME# sense | 23 | |
| 3.3 V sense | 38 | 10 kΩ series, 100 kΩ to GND |
| COM UART TX / RX | 2 / 46 | Debug Probe (kernel debugger) |
| ESP32 UART0 | devkit USB | `oxctl`, 921600 |

## 5. Pins in use

**UPduino**: 2, 4, 14–17 (flash), 20 (12 MHz), 23, 25, 26, 27, 31, 32, 34, 36, 37, 38, 39–41 (LED), 42, 43, 44, 45, 46, 47, 48. Pin 35 is unusable while the PLL runs. Free: 3, 6, 9, 10, 11, 12, 13, 18, 19, 21, 28.

**ESP32 devkit**: IO1/IO3 (USB UART), 19, 20, 21, 22, 25, 26, 27, 32, 34, 35, 36. Free: 4, 7, 8, 13, 14, 33, 37, 38, 39 (and strapping pins 0, 2, 5, 12, 15 with care).
