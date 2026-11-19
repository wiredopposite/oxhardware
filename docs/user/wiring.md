# Wiring

The bench setup: a UPduino v3.1 for the FPGA and an ESP32-PICO-DevKitM-2 (ESP32-PICO-MINI-02, 8MB flash, 2MB PSRAM) for the ESP32, wired to a 1.6 console. Earlier revisions aren't supported yet, see [Status](../dev/status.md).

UPduino pins are given by their silkscreen label, which is the FPGA package pin. The flash bus and FPGA control pins have function labels instead.

## Power and ground

| Connection | Notes |
|---|---|
| UPduino GND, ESP32 devkit GND, Xbox GND | One short common ground. Each board is powered from its own USB cable |
| 3.3V rails | Not connected between the boards |

All signals are 3.3V on both sides.

Power the UPduino before the console and turn the console off first. Ground the UPduino to the console at the LPC header with a short wire: with a long ground return, `LRESET#` picks up glitches.

## LPC header

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | LCLK | 2 | GND |
| 3 | LFRAME# (not connected on 1.3 to 1.5) | 4 | Power button (1.6 only) |
| 5 | LRESET# | 6 | 5V (1.6: always on with AC) |
| 7 | LAD3 | 8 | LAD2 |
| 9 | 3.3V (not connected on 1.6) | 10 | LAD1 |
| 11 | LAD0 | 12 | GND |
| 13 | SMBus SCL | 14 | SMBus SDA |
| 15 | 3.3V | 16 | SERIRQ (not connected on 1.6, which has it on a test point under the board) |

Find pin 1 on the board before wiring. Use pin 15 for 3.3V on every revision.

## ESP32 to FPGA link

SPI mode 0, with the ESP32 as master. The module has no GPIO18 or GPIO23 (they're wired to its flash), so the link goes through the GPIO matrix and runs at 20MHz at most.

| Signal | UPduino | ESP32 devkit | Notes |
|---|---|---|---|
| `LINK_SCK` | 44 | IO21 (J2-2) | A global-buffer input on the FPGA |
| `LINK_MOSI` | 45 | IO22 (J2-3) | |
| `LINK_MISO` | 47 | IO19 (J2-4) | The FPGA floats it while `LINK_CS_N` is high |
| `LINK_CS_N` | 48 | IO20 (J2-1) | |
| `FPGA_IRQ_N` | 4 | IO34 (J2-13) | An input-only pin on the ESP32. The FPGA drives it push-pull |

## Config bus and FPGA control

These let the ESP32 hold the FPGA in reset and program its flash. The UPduino's FT232H is on the same nets and lets go of them when `iceprog` isn't running, so never run `iceprog` while the ESP32 is using the flash.

| Signal | UPduino | ESP32 devkit | Notes |
|---|---|---|---|
| `CFG_SS_N` | SS | IO32 (J3-6) | |
| `CFG_SCK` | SCK | IO26 (J3-5) | |
| `CFG_MOSI` (ESP32 to flash) | SI | IO27 (J3-8) | Labelled from the flash's side: serial in |
| `CFG_MISO` (flash to ESP32) | SO | IO36 / SENSOR_VP (J3-2) | Serial out. An input-only pin on the ESP32 |
| `FPGA_CRESET_N` | RST | IO25 (J3-4) | Open-drain from the ESP32. Low holds the FPGA in reset |
| `FPGA_CDONE` | DONE | IO35 (J2-12) | An input-only pin on the ESP32 |

SS, SCK, SI and SO are the last four pins of the UPduino's bottom header row, after `20` (FPGA pins 16, 15, 14 and 17). RST and DONE are on the top row, between `VIO` and `R`.

None of the ESP32's strapping pins (IO0, IO2, IO5, IO12, IO15) are used, so an unconfigured FPGA's pull-ups can't change how the ESP32 boots.

## Console

| Signal | UPduino | Notes |
|---|---|---|
| LCLK | 37 | 33Ω. 100kΩ to GND on the FPGA side, since nothing drives it while the console is off |
| LRESET# | 32 | 100Ω. 100kΩ to GND on the FPGA side |
| LAD0 / LAD1 / LAD2 / LAD3 | 34 / 43 / 36 / 42 | 33Ω |
| LFRAME# MOSFET gate | 31 (and 25, 26, 27) | See [below](#lframe-switch-16) |
| LFRAME# sense | 23 | |
| 3.3V sense | 38 | 10kΩ series, 100kΩ to GND |
| COM UART TX / RX | 2 / 46 | To a 3.3V USB serial adapter, for the kernel debugger |
| USB D+ / D- | 19 / 18 | 27Ω each, to the console's controller port |
| USB pull-up | 13 | 1.5kΩ from pin 13 to D+ |

| Signal | Console | ESP32 devkit | Notes |
|---|---|---|---|
| SMBus SDA | LPC header 14 | IO33 (J3-7) | The console's pull-ups only |
| SMBus SCL | LPC header 13 | IO13 (J3-11) | |
| Power button | Front panel, power switch line | IO4 | Open-drain from the ESP32, low is pressed |
| Eject button | Front panel, eject switch line | IO8 | The same |

The ESP32's own UART0 is the devkit's USB port, which `oxctl` uses at 921600.

### LFRAME# switch (1.6)

On a 1.6 the FPGA has to hold `LFRAME#` low to hide its reads from the onboard flash, and an iCE40 pin can't pull it low against the MCPX. It switches a logic-level N-MOSFET instead, such as an IRLML6244 (SOT-23):

| MOSFET pin | Connects to |
|---|---|
| Gate (1) | The FPGA's gate output, with 10kΩ to source |
| Source (2) | Console GND at the LPC header |
| Drain (3) | `LFRAME#`, header pin 3 |

`LFRAME#` also goes to the sense input through its series resistor. With the console off and the gate forced on (`fpga/tools/gate.py` with the `lpc_serve` design), `LFRAME#` should measure a few ohms to GND. With the gate off, it shouldn't.

## Pins in use

**UPduino:** 2, 4, 13, 14 to 17 (flash), 18, 19, 20 (12MHz), 23, 25, 26, 27, 31, 32, 34, 36, 37, 38, 39 to 41 (LED), 42 to 48. Pin 35 can't be used while the PLL runs. Free: 3, 6, 9, 10, 11, 12, 21, 28.

**ESP32 devkit:** IO1 and IO3 (USB UART), 4, 8, 13, 19, 20, 21, 22, 25, 26, 27, 32, 33, 34, 35, 36. Free: 7, 14 (pulses during boot), 37, 38, 39, and the strapping pins 0, 2, 5, 12 and 15 with care.

The UPduino's 12MHz oscillator reaches pin 20 only with R16 "OSC" shorted.
