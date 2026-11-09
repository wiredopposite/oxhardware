# 04 — Hardware Notes, Pin Budgets, Bring-up

Firmware-facing hardware assumptions, not a schematic. Final pin maps go in `fpga/boards/<board>/*.pcf` and an ESP32 board header once checked against datasheets.

## 1. FPGA pin budget (iCE40UP5K-SG48, 39 I/O)

| Group | Signals | Count | Constraints |
|---|---|---|---|
| LPC | LAD0–3, LCLK, LRESET#, LFRAME#, D0_N | 8 | LCLK on a global-buffer input; LFRAME#/D0_N open-drain capable; VCCIO 3.3 V |
| Config bus / NOR | SPI_SCK, SPI_SS, SPI_SO, SPI_SI (fixed config pins 15/16/14/17) + NOR IO2, IO3 | 6 | Shared with ESP32 HSPI (ESP32 Hi-Z unless FPGA in reset) |
| ESP32 link | SCK, MOSI, MISO, CS_N, IRQ_N | 5 | Separate from the config bus (D3) |
| Clock | 12 MHz in | 1 | Global-buffer input feeding `SB_PLL40_CORE` (UPduino: pin 20 = IOB_25B_G3, R16 "OSC" shorted) |
| USB | DP, DN, PU | 3 | Adjacent, same bank |
| COM UART | TX, RX, RTS, CTS | 4 | → bridge channel B |
| RGB LED | RGB0–2 | 3 | Dedicated current-sink pins |
| Debug | 2 spare outputs on a header | 2 | LPC state / USB strobes for the logic analyser |
| **Total** | | **32 of 39** | SD moved to the ESP32 (D8); 7 spare |

**UPduino v3.1 (dev board)**: flash is a **W25Q32 (4 MB, JEDEC `EF 40 16`)** with only SO/SI connected (no IO2/IO3), so `NOR_WIDTH=1`. Header labels are the FPGA package pin numbers. The RGB LED and config-bus flash are on-board; the 12 MHz oscillator reaches the FPGA (pin 20) only with R16 "OSC" shorted. The on-board FT232H is a single-channel SPI programmer, not a UART to the FPGA: use a separate 3.3 V USB-UART on two GPIOs during bring-up.

`CRESET_B` and `CDONE` are dedicated pins (not counted). **Pin 35 cannot be used as I/O while the PLL is in use** (it shares the PLL output's I/O site on SG48), so the final pin map must leave it free.

## 2. ESP32 pin budget (ESP32-PICO-MINI-02)

| Group | Signals | Count | Proposed | Constraints |
|---|---|---|---|---|
| Programming/console | U0TXD, U0RXD, EN, GPIO0 | 4 | 1, 3, EN, 0 | Standard DTR/RTS auto-reset |
| Link | SCK, MOSI, MISO, LINK_CS | 4 | 18, 23, 19, 5 (VSPI IO_MUX) | Dedicated to the FPGA. GPIO5 is a strap: the FPGA's unconfigured pull-up holds it high, which is its default |
| SD + LCD bus | SCK, MOSI, MISO, SD_CS, SD_CD, LCD_CS, LCD_DC, LCD_BL | 8 | HSPI via GPIO matrix (≤ 26 MHz is fine for both) | **Not** GPIO12/15/2. SD MISO pull-up 10 kΩ |
| FPGA control | CRESET_N, CDONE, IRQ_N | 3 | CDONE/IRQ_N on input-only 34–39 | |
| Config bus | SCK, MOSI, MISO, SS | 4 | HSPI re-pinned via GPIO matrix (recovery only), **not** GPIO12/15/2 | Separate wires from the SD/LCD bus. An unconfigured iCE40 pulls its pins high; GPIO12 high at reset = 1.8 V flash = no boot. Burn the VDD_SDIO eFuse to 3.3 V on production boards regardless. 33 Ω series. |
| NeoPixel | DATA0, DATA1 | 2 | any | 74AHCT1G125 to 5 V |
| Standby extras (opt.) | POWER_BTN, EJECT_BTN, STANDBY_DETECT | 3 | | Only meaningful with standby |
| **Total** | | **25 (+3)** | of ~27 usable | Tight. If short (e.g. with standby extras): read CDONE via the link `ID` instead, poll SD presence instead of SD_CD, tie LCD_BL on |

## 3. Power (no standby required)

| Rail | Source | Notes |
|---|---|---|
| 5 V in | **Console 5 V**: controller-port VBUS from the same test pads as D+/D− (preferred), or LPC-header pin 6 (5 V). **Optional** standby 5 V on its own connector, OR-ed via ideal-diode/Schottky. | Q4: confirm current available on each |
| 3.3 V | Own buck/LDO from 5 V, ≥ 800 mA | ESP32 RF peaks ~500 mA; FPGA, NOR, logic |
| 1.2 V | LDO ≥ 100 mA | iCE40 VCC, VCCPLL (RC filtered) |
| VPP_2V5 | Tied to 3.3 V | NVCM is never programmed |
| NeoPixel 5 V | From 5 V in, polyfuse, firmware current limit | Strips dominate the budget; without standby they share console USB power — keep defaults dim |

- **Rise time matters**: the FPGA must configure (20–70 ms) before the MCPX leaves reset. Our 3.3 V must come up fast from console 5 V — no soft-start of hundreds of ms on the regulator.
- Back-powering: our board and the LPC bus come from the same console rails, so they rise/fall together. If standby powers us while the console is off, the FPGA's LPC pins must be Hi-Z — guaranteed only while `SERVE_EN`-driven outputs are gated by `LRESET#`/LCLK-present; add series resistors on LAD/LCLK/LRESET# regardless (§4).
- iCE40 I/O are not 5 V tolerant.
- USB-UART bridge powered from the PC's VBUS; its UART lines must not back-power the board (bridge VIO from board 3.3 V, or series resistors).

## 4. Xbox connections

| What | Wires | Notes |
|---|---|---|
| LPC header | LAD0–3, LCLK, LRESET#, LFRAME#, 3.3 V (sense/ref), GND | 33 Ω series on LAD0–3/LCLK, 100 Ω on LRESET#; keep the LCLK stub short and the ground return solid (§4.3). Pinout in §4.1 |
| D0 (1.0–1.5) | 1 | To `D0_N` (open-drain) — releasable for TSOP fallback — or grounded |
| 1.6 | LFRAME# via N-MOSFET (§4.2) | Requires the LPC header rebuild |
| Controller-port test pads | D+, D−, VBUS, GND | 27 Ω series, 1.5 kΩ pull-up via `USB_PU`, ESD array. Only one port → multiple devices via composite or hub emulation |
| Standby (optional) | 5 V SB (+ power/eject button wires) | Enables wake-from-controller, ROM switch + power-cycle |

### 4.1 LPC debug header pinout (xboxdevwiki "LPC Debug Port")

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | LCLK | 2 | GND |
| 3 | LFRAME# — **not connected on 1.3–1.5** | 4 | PWR button (1.6 only) |
| 5 | LRESET# | 6 | 5 V (1.6: always on with AC) |
| 7 | LAD3 | 8 | LAD2 |
| 9 | 3.3 V — **not connected on 1.6** | 10 | LAD1 |
| 11 | LAD0 | 12 | GND |
| 13 | SCL (SMBus) | 14 | SDA (SMBus) |
| 15 | 3.3 V | 16 | SERIRQ (1.0) |

Find pin 1 on the actual board before wiring. 1.5 boards reportedly also lack header GND/3.3 V (XBlast Lite manual) — take them from nearby points. Use pin 15 for 3.3 V on all revisions.

### 4.2 1.6 LFRAME# switch

The FPGA cannot overdrive the MCPX on `LFRAME#` (02-fpga §4.4), so it switches an N-MOSFET:

| MOSFET (SOT-23, e.g. IRLML6344) | Connects to |
|---|---|
| Gate (1) | FPGA abort output; 10 kΩ to source |
| Source (2) | Console GND at the LPC header |
| Drain (3) | `LFRAME#` (header pin 3) |

`LFRAME#` is also wired, through its series resistor, to an FPGA input for sensing. With the console off and the gate forced on, `LFRAME#` should measure a few ohms to GND; with the gate off it should not.

### 4.3 Dev setup (UPduino v3.1)

| Signal | FPGA pin | Notes |
|---|---|---|
| LCLK | 37 | |
| LRESET# | 32 | 100 Ω; filtered in RTL (16 LCLK) |
| LAD0 / LAD1 / LAD2 / LAD3 | 34 / 43 / 36 / 42 | 33 Ω |
| LFRAME# gate | 31 (+ 25 / 26 / 27 optional, same signal) | To the MOSFET gate |
| LFRAME# sense | 23 | |
| 3.3 V sense | 38 | 10 kΩ series, 100 kΩ to GND at the pin |
| UART TX / RX | 2 / 46 | 3.3 V USB-UART |

Power the UPduino (USB) before the console and switch the console off first. Ground the UPduino to the console at the LPC header with a short wire: on a long ground return `LRESET#` picks up glitches (thousands per boot were rejected by the 16-clock filter).

## 5. Bring-up plan

| Step | Hardware | Goal | Status |
|---|---|---|---|
| A | **UPduino v3.1** + a 3.3 V USB-UART | Toolchain, clocks, UART | Done |
| B | UPduino on the LPC header, passive (`lpc_probe`, `lpc_iolog`) | Cold-boot timing (00-system §4.4); legacy register traffic (02-fpga §6.2) | Done on 1.6 |
| C | UPduino serving from its flash (`lpc_serve`) with the LFRAME# switch | Boot a ROM on a 1.6 | Done |
| D | + COM 16550 and Super I/O | Devkit kernel debug output on the PC | |
| E | + boot config, ESP32 devkit on the link pins and config bus | ESP32 programs flash, resets FPGA, link at 20 → 40 MHz; bank switching | |
| F | + controller-port pigtail | XID enumeration; composite vs. hub experiment; BT playing | |
| G | Custom PCB v1 | Everything; 1.0–1.5 via D0; SD; LCD | |

An **LPC host emulator** (e.g. an RP2040/RP2350 PIO program or a second FPGA) that issues scripted LPC cycles would let the target be regression-tested on the bench without a console.

## 6. BOM sketch (v1)

| Part | Example | Note |
|---|---|---|
| FPGA | iCE40UP5K-SG48I | |
| MCU | ESP32-PICO-MINI-02-N8R2 | |
| NOR | W25Q128JVSIQ (16 MB QSPI, 4 KB sectors, suspend) | Bitstream + boot config + 15 MB of ROM banks |
| USB-UART | CH342F (dual) or CP2105 | A → ESP32 (auto-reset), B → FPGA COM with RTS/CTS |
| Oscillator | 12 MHz 3.3 V CMOS | |
| Regulators | 3.3 V buck ≥ 800 mA (fast start), 1.2 V LDO | |
| microSD socket | push-push with card detect | On ESP32 HSPI (shared with LCD) |
| Level shifters | 74AHCT1G125 ×2 | NeoPixel data |
| Protection | USBLC6-2 ×2, polyfuse on strip 5 V, ideal diode/Schottky for standby OR-ing | |
| Connectors | USB-C (PC), LPC header, controller-port pigtail, optional standby header, 2× strip, LCD FPC | |
