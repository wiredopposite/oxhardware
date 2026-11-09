# 05 — Dev Board Wiring (UPduino v3.1 + ESP32-PICO-DevKitM-2)

Bench setup for phases 0–1: the UPduino stands in for the FPGA side, the ESP32-PICO-DevKitM-2 (ESP32-PICO-MINI-02, 8 MB flash, 2 MB PSRAM) for the ESP32 side. Final-board pin choices are in [04-hardware.md](04-hardware.md); the signals are defined in [01-interface.md](01-interface.md).

UPduino pins are given by their silkscreen label: a number is the FPGA package pin. The flash bus and FPGA control pins have function labels instead, at the ends of the two header rows (§3).

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

Lets the ESP32 hold the FPGA in reset and program its flash directly (01-interface §2.2). The UPduino's FT232H is on the same nets; it releases them when `iceprog` is not running, so **never run `iceprog` and an ESP32 flash operation at the same time.**

| Signal | UPduino | ESP32 devkit | Notes |
|---|---|---|---|
| `CFG_SS_N` | **SS** | IO32 (J3-6) | |
| `CFG_SCK` | **SCK** | IO26 (J3-5) | |
| `CFG_MOSI` (ESP32 → flash) | **SI** | IO27 (J3-8) | Labelled from the flash's side: flash serial *in* |
| `CFG_MISO` (flash → ESP32) | **SO** | IO36 / SENSOR_VP (J3-2) | Flash serial *out*. ESP32 input-only pin |
| `FPGA_CRESET_N` | **RST** | IO25 (J3-4) | ESP32 drives open-drain; low = FPGA held in reset |
| `FPGA_CDONE` | **DONE** | IO35 (J2-12) | ESP32 input-only pin |

Where they are: **SS, SCK, SI, SO** are the last four pins of the UPduino's bottom header row, after `20` (FPGA pins 16, 15, 14, 17). **RST** and **DONE** are on the top row between `VIO` and `R`.

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
