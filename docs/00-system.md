# 00 — System Specification

## 1. Goal

A modchip for the original Xbox built from an **iCE40UP5K FPGA** and an **ESP32-PICO-MINI-02**:

- The FPGA does everything with hard timing, and **boots on its own** fast enough to serve the BIOS on a cold boot: LPC target (BIOS ROM, COM port, legacy modchip registers, flash-chip emulation), USB 1.1 full-speed device PHY/SIE, QSPI NOR controller.
- The ESP32 does everything with policy, protocol or radios: manages the ROM library, implements legacy-modchip "personalities", runs the USB device stack (incl. hub emulation), serves the SD card as USB mass storage, Bluetooth gamepads, NeoPixels and LCD.

## 2. Requirements

### 2.1 Power

| ID | Requirement |
|---|---|
| P-1 | **Must work with no standby power** — board powered only while the console is on (LPC header and/or controller-port 5 V). |
| P-2 | Standby 5 V is an **optional** extra wire; when present it enables extras (§4.3) but nothing core depends on it. |

### 2.2 FPGA (iCE40UP5K)

| ID | Requirement | Phase |
|---|---|---|
| F-BOOT-1 | Self-configure from its own QSPI NOR on power-up and serve the boot bank **without the ESP32** | 1 |
| F-LPC-1 | LPC target answering memory reads with `A[31:24] = 0xFF` | 1 |
| F-LPC-2 | BIOS window sizes 256 KB, 512 KB, 1 MB; smaller images mirror through the 1 MB window and the whole `0xFF` 16 MB region | 1 |
| F-LPC-3 | ROM data from a runtime-selectable bank in NOR; bank set by boot config, ESP32, or legacy bank-select writes | 1 |
| F-LPC-4 | Bank switch can be armed to take effect at the next `LRESET#` assertion (hardware) | 1 |
| F-LPC-5 | 1.0–1.5: drive `D0` low (open-drain) while serving; release to fall back to TSOP | 1 |
| F-LPC-6 | 1.6: `LFRAME#` transaction-abort mode | 2 |
| F-LPC-7 | Never wedge the bus: SYNC timeout → SYNC error + release | 1 |
| F-COM-1 | 16550-compatible COM port on LPC I/O (default 0x3F8), ox semantics (guest baud ignored, LSR back-pressure) | 1 |
| F-COM-2 | COM bridged to a physical UART (dual USB-UART channel B), fixed programmable baud, optional RTS/CTS | 1 |
| F-COM-3 | Optional route of COM to the ESP32 instead | 2 |
| F-COM-4 | SMSC-style Super I/O config space (0x2E/0x2F) | 2 |
| F-LEG-1 | **Generic I/O trap**: programmable LPC I/O port windows; writes captured in order into a deep FIFO for the ESP32, reads served from ESP32-maintained shadow registers (optionally deferred to the ESP32) — the basis for emulating legacy modchips (Xenium, SmartXX, X3, Aladdin… LCD / LED / bank registers) | 3 |
| F-LEG-2 | Hardware fast-path for legacy **bank-select** writes (e.g. Xenium `0xEF`) via a bank map table, works before the ESP32 is up | 3 |
| F-LEG-3 | **JEDEC flash-chip emulation** on LPC memory writes to the BIOS window, backed by the NOR, so existing Xbox flashers can write ROMs | 3 |
| F-MBX-1 | LPC I/O mailbox (byte FIFOs both ways) for a native Xbox-side protocol (ROM management, ESP32 OTA) | 3 |
| F-USB-1 | USB 1.1 FS device: PHY, NRZI, bit-stuff, CRC5/16, auto handshake, data toggles | 2 |
| F-USB-2 | **Multiple device addresses** on one port (hub + downstream devices, pico_usb model) and pooled endpoint buffers | 2 |
| F-USB-3 | ESP32 services endpoints over SPI with relaxed latency; FPGA NAKs until a buffer is armed | 2 |
| F-LED-1 | Status on the UP5K's RGB current-driver pins | 1 |

### 2.3 ESP32 (ESP32-PICO-MINI-02)

| ID | Requirement | Phase |
|---|---|---|
| E-BOOT-1 | Programmable through a USB-UART bridge with DTR/RTS auto-reset (devkit style) | 0 |
| E-FPGA-1 | Program/recover the FPGA NOR (bitstream, ROM banks, boot config) via the shared config bus while holding the FPGA in reset; slave-configure the FPGA as fallback | 0 |
| E-ROM-1 | ROM library management in NOR: catalogue, add (PC / Xbox mailbox / flasher emulation), delete, verify, select default | 1→3 |
| E-ROM-2 | Runtime ROM selection: boot config default, CLI, gamepad hotkey, Xbox mailbox, legacy bank writes, LCD | 1→3 |
| E-USB-1 | USB device stack: **pico_usb** core (from `refs/ox/thirdparty/pico_usb`) with a new FPGA DCD; ox's XID and MSC functions; hub emulation | 2 |
| E-BT-1 | Bluetooth gamepads via BlueRetro (`refs/oxblueretro`), mapped to `GamepadIn`/`GamepadOut` | 2 |
| E-LEG-1 | Legacy personality layer: decodes trapped I/O traffic (e.g. HD44780 LCD writes, LED registers) and renders it to whatever is attached (SPI LCD, NeoPixels) | 3 |
| E-MSC-1 | SD card (SPI mode) on the ESP32's second SPI bus, accessed as raw blocks only; exposed to the Xbox as USB mass storage (SCSI/BOT) through the FPGA USB. The ESP32 never mounts it while it is presented over USB | 4 |
| E-LED-1 | 1–2 WS2812 strips (RMT) | 1 |
| E-LCD-1 | SPI LCD status/menu + legacy LCD rendering | 3 |

### 2.4 Non-goals (for now)

SMBus device emulation (needs another wire — revisit with the standby option), HDMI/video, ESP32 mounting the SD card while it is presented to the Xbox. (Optional later: ESP32 imports ROMs from the card's FAT while the MSC function is detached — strictly one owner at a time.)

## 3. Architecture

```
                         ┌─────────────────────────────── iCE40UP5K ───────────────────────────────┐
 Xbox LPC header         │ ┌────────────┐  rd/wr req  ┌──────────────┐  QSPI  ┌─────────────────┐    │
 LAD[3:0] LCLK LRESET# ─►│ │ lpc_target │────────────►│ nor_ctrl     │◄──────►│ QSPI NOR 16 MB  │◄─┐ │
 LFRAME# D0            ◄─│ │ (LCLK dom.)│◄────────────│ +prefetch    │ config │ bitstream, boot │  │ │
                         │ └┬──┬──┬──┬──┘             │ +flash_emu   │  pins  │ cfg, ROM banks  │  │ │
                         │  │  │  │  └─ mem writes ──►│ +arbiter     │        └─────────────────┘  │ │
                         │  │  │  └──► com16550 ◄──► uart_phy ───────► TX/RX/RTS/CTS ──► USB-UART B │ │
                         │  │  └─────► superio, mailbox                                          │ │
                         │  └────────► iotrap (windows, shadow RAM, event FIFO in SPRAM, bankmap) │ │
 Controller-port pads    │ ┌──────────────┐ EP slot pool (EBR)                                  │ │
 USB D+/D- (5 V) ◄──────►│ │ usb phy+sie  │◄─────────────┐                                      │ │
                         │ │ multi-addr   │              │      ┌──────────┐                    │ │
                         │ └──────────────┘        ┌─────┴────┐                                    │ │
                         │  RGB LED drv ◄──────────│ link +   │                                    │ │
                         │  12 MHz → PLL 48 MHz    │ regfile  │                                    │ │
                         │                         └────┬─────┘                                  │ │
                         └──────────────────────────────┼────────────────────────────────────────┘ │
                                   link SPI + IRQ_N     │        CRESET_B, CDONE, config-bus SPI    │
                         ┌──────────────────── ESP32-PICO-MINI-02 ────────────────────────────────┐│
 USB-UART A ────────────►│ UART0 flash/log/CLI   VSPI: link only   HSPI: SD + LCD ⇄ config bus*  ├┘
 microSD, SPI LCD ◄─────►│ core 0: BlueRetro BT    core 1: fpga_svc, pico_usb, rom_mgr, legacy,   │
 BT gamepads ))))        │                                 msc_sd, led, cli, lcd                  │
 NeoPixel ×2 ◄───────────│ RMT                                                                    │
                         └────────────────────────────────────────────────────────────────────────┘
 * HSPI normally runs the SD/LCD pins; it is re-initialised onto the config-bus pins only for
   factory programming / recovery (FPGA held in reset). 03-esp32 §7.1.
```

### 3.1 Key decisions

| # | Decision | Status | Rationale |
|---|---|---|---|
| D1 | **ROMs live in a QSPI NOR on the FPGA's config bus; the FPGA self-boots from it and serves ROMs straight from it.** PSRAM dropped. ESP32 flash holds only firmware (+ a golden bitstream for recovery). | [DECIDED — replaces "ROMs in ESP32 flash"] | The UP5K needs *some* external memory regardless (128 KB SPRAM < smallest 256 KB BIOS), so it's NOR vs. PSRAM, not NOR vs. nothing. NOR: same part count, non-volatile, holds the bitstream too, no ESP32 on the boot path (P-1, §4), flasher emulation is natural. |
| D2 | ESP32 is wired to the config bus too: it can hold `CRESET_B` low and program the NOR directly (factory programming, unbrick), or slave-configure the FPGA after putting the NOR in deep power-down | [PROPOSED] | No external programmer ever needed; a bad bitstream can't brick the board. |
| D3 | ESP32↔FPGA link uses **separate** FPGA pins from the config bus | [PROPOSED] | After boot the FPGA is master of the NOR on the config pins; the ESP32 needs its own always-available slave link. |
| D4 | USB: FPGA PHY+SIE with **multiple device addresses and a pooled EP buffer table**; ESP32 runs **pico_usb** with a new DCD | [PROPOSED — replaces TinyUSB] | Only one physical port (MCPX test pads). pico_usb already does composite *and* hub emulation (hub + N downstream devices sharing one wire) and ox's XID/MSC functions are written against it. |
| D5 | LPC on `LCLK` directly; everything else on 48 MHz PLL from a 12 MHz oscillator | [PROPOSED] | As EOS; USB needs an accurate 48 MHz. |
| D6 | COM: ox semantics — guest divisor stored but ignored, LSR back-pressure, physical UART on its own bridge channel at a fixed programmable baud | [PROPOSED, per ox] | No data loss regardless of guest settings; works for kernel debugging without ESP32 involvement. |
| D7 | Legacy modchip compatibility = **generic FPGA I/O trap + ESP32 personalities**, plus two hardware fast-paths (bank select, JEDEC flash) | [PROPOSED] | Keeps gateware generic; new personalities are firmware-only. |
| D8 | SD card on the **ESP32's** second SPI bus (shared with the LCD), raw block access, presented to the Xbox only as USB mass storage | [DECIDED] | The USB stack is on the ESP32, so every MSC block passes through it anyway; this way blocks cross the link once instead of twice. SPI SD (~2 MB/s) exceeds USB FS bulk (~1 MB/s). Saves FPGA LUTs/EBR and 5 FPGA pins; ESP-IDF `sdspi` does the card protocol. Link bus stays dedicated to the FPGA (USB latency). |
| D9 | HDL Verilog (Yosys subset); Yosys + nextpnr-ice40 + icepack; cocotb sim. ESP-IDF pinned to BlueRetro's version. | [PROPOSED] | |
| D10 | First dev board: **UPduino v3.1** (UP5K-SG48, 4 MB flash on the config pins, 12 MHz osc). RTL stays vendor-neutral outside `boards/` so a fallback FPGA (02-fpga §11.1) stays possible | [DECIDED] | Hardware on hand; same FPGA as the target. |

## 4. Cold boot without standby (P-1)

### 4.1 The problem

Without standby, the board powers up with the console. The MCPX starts fetching from LPC shortly after `LRESET#` deasserts. If nobody answers:
- 1.0–1.5 with `D0` driven by us: D0 is released (FPGA unconfigured = I/O floating) → console boots its TSOP. Wrong ROM, but not a failure.
- 1.0–1.5 with D0 grounded, or 1.6: no BIOS → boot failure (FRAG, or the SMC retries).

### 4.2 Budget

| Path | Ready after power-good | Verdict |
|---|---|---|
| **Original plan**: ESP32 boots → slave-configures FPGA → copies ROM to PSRAM | Default IDF config: a few hundred ms to `app_main` ⇒ **~0.3–0.8 s** total. Tuned (skip image validation on power-on, logs off, QIO 80 MHz): ~50–100 ms to `app_main` + config ~35 ms + copy 15–50 ms ⇒ **~100–200 ms** [measure] | Might work; depends entirely on the unmeasured Xbox timing, and every cold boot is a full reload. Fallback only if a PSRAM design is ever wanted. |
| **D1**: FPGA self-configures from NOR, reads boot config, serves directly from NOR | POR + config of ~104 KB at the configured SPI master clock ⇒ **~20–70 ms** [VERIFY on hardware] | Same class as EOS (Gowin, own flash, LPC 3.3 V only) and ox (RP2350 XIP), both of which work on real consoles. |

For comparison: ox and Stellar (GW1NR-4 + RP2040) get away without a dedicated ROM chip because their MCU (RP2040/RP2350) is running within milliseconds, and the GW1NR has config flash and PSRAM in-package. The slow element in our system is the ESP32 boot chain, not the FPGA.

The ESP32 boots in parallel and attaches when ready (USB, BT, legacy personalities). Nothing it does is on the boot-critical path. Legacy I/O writes that happen before it's up are queued in the FPGA event FIFO (SPRAM, 8 K entries); reads are served from the boot-config shadow values.

### 4.3 What standby adds (optional wire)

- ESP32 + BT alive while console is off → controllers stay paired; **power on the console from a BT controller** (also needs a power-button wire — optional header).
- ROM switch + automatic power-cycle; NeoPixel effects while off; ESP32 can rewrite the NOR with the console off.
- Board design: OR-ing (ideal diode) between standby 5 V and console 5 V; firmware detects which is present.

### 4.4 Gating experiment (phase 0)

Measure on a 1.0–1.5 and a 1.6 board with a logic analyser: t(LPC 3.3 V → `LRESET#` high), t(→ first LPC memory read), and what the MCPX does on long-wait SYNC (how long it tolerates a stall). This confirms D1's margin and tells us whether serve-while-filling stalls are usable.

No logic analyser needed: a USB-powered UPduino running a small `lpc_probe` design (passive inputs on 3.3 V-sense, `LRESET#`, `LFRAME#`, `LCLK`, via series resistors) timestamps rail-up → `LRESET#` → first START with the 48 MHz clock and prints the result over UART. Our own self-boot time (power → `CDONE` → LPC-ready) is measured separately on the bench with the ESP32 devkit timing GPIO edges. The long-wait tolerance can only be measured once we serve (phase 1). Not a blocker: on 1.0–1.5 a too-slow chip falls back to the TSOP, which is safe.

## 5. Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **0 – Bring-up & measure** | Repo skeletons, FPGA build for UPduino v3.1, ESP32 programs NOR + resets FPGA, FPGA self-boots, link with `ID`/scratch; cold-boot timing measurements (§4.4) | ESP32 writes a bitstream to NOR, FPGA boots it, link loopback 10⁶ transfers error-free; timing numbers recorded |
| **1 – BIOS + COM** | NOR controller + prefetch, LPC target, boot config, 16550 + UART, ESP32 ROM library in NOR + CLI, NeoPixels | 1.0–1.5 (D0) cold-boots 256 K / 512 K / 1 MB banks with no standby; switch at reset; kernel debug output on PC COM port |
| **2 – USB + BT + 1.6** | Multi-address USB SIE, pico_usb FPGA DCD, XID, hub emulation, BlueRetro, LFRAME# mode, Super I/O | BT controller plays through the test-pad port; 1.6 cold-boots; composite vs. hub result recorded (Q2) |
| **3 – Legacy + console-side management** | iotrap + bank map + JEDEC flash emulation, mailbox, first personality (Xenium: bank + LED + LCD), SPI LCD | An existing Xbox flasher writes a ROM into a bank; XeniumOS-style LCD/LED output appears on our LCD/NeoPixels |
| **4 – SD mass storage** | `msc_sd` on the ESP32 (`sdspi` raw blocks) behind ox's MSC function over the FPGA USB | Xbox reads/writes the SD card (e.g. as an MU/USB drive) and can upload ROMs from it |

## 6. Open questions

| # | Question | Default |
|---|---|---|
| Q1 | ~~Confirm D1~~ — confirmed. | — |
| Q7 | If the UP5K turns out inadequate (LUTs, timing), which fallback FPGA? See 02-fpga §11.1 | Decide on real Yosys numbers; GW1N-9 (no PSRAM) or iCE40HX4K, both keeping the NOR architecture |
| Q2 | Multi-controller on one root port: composite (4 XID interfaces, as ox's default) vs. hub emulation (Duke-like: hub + XIDs + MU). Which does the stock kernel map to players 1–4, and does it accept an MU/MSC device? Was ox's 4-gamepad composite proven on real hardware? | Build both (cheap in this design), decide by experiment |
| Q3 | Which legacy chips matter first? (Xenium/OpenXenium, SmartXX, Xecuter 3, Aladdin…) and which flasher(s) the JEDEC emulation must satisfy (Raincoat, XBlast, EvoX, XeniumOS…) — determines the ID code(s) to report | Xenium first; ID of an SST49LF080A-class part [VERIFY] |
| Q4 | 5 V for NeoPixels/board without standby: controller-port VBUS (current limit?) or an LPC-header 5 V pin? 3.3 V current available from the LPC header? | Board powered from controller-port 5 V via own regulator [VERIFY limits] |
| Q5 | Mailbox I/O base & protocol — reuse ox's console-port protocol so ox Xbox-side tools work? | Reuse ox's |
| Q6 | Licence | Decide later; avoid pasting LGPL/GPL code meanwhile (pico_usb, BlueRetro are Apache-2.0; valentyusb BSD-3) |
