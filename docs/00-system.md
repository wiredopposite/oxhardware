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
| F-LPC-6 | 1.6: `LFRAME#` transaction abort (overrides the onboard flash) through an external N-MOSFET, with a boot hold from reset until the first fetch sequence is served (02-fpga §4.4) | 1 |
| F-LPC-7 | Never wedge the bus: SYNC timeout → SYNC error + release | 1 |
| F-COM-1 | 16550-compatible COM port on LPC I/O (default 0x3F8); guest baud ignored, LSR back-pressure instead of dropping data | 1 |
| F-COM-2 | COM bridged to a physical UART (dual USB-UART channel B), fixed programmable baud, optional RTS/CTS | 1 |
| F-COM-3 | Optional route of COM to the ESP32 instead | 2 |
| F-COM-4 | SMSC-style Super I/O config space (0x2E/0x2F), enough for the kernel's serial-debug probe (02-fpga §5.3) | 1 |
| F-LEG-1 | **Generic I/O trap**: programmable LPC I/O port windows; writes captured in order into a deep FIFO for the ESP32, reads served from ESP32-maintained shadow registers (optionally deferred to the ESP32) — the basis for emulating legacy modchips (Xenium, SmartXX, X3, Aladdin… LCD / LED / bank registers) | 3 |
| F-LEG-2 | Hardware fast-path for legacy **bank-select** writes (e.g. Xenium `0xEF`) via a bank map table, works before the ESP32 is up | 3 |
| F-LEG-3 | **JEDEC flash-chip emulation** on LPC memory writes to the BIOS window, backed by the NOR, so existing Xbox flashers can write ROMs | 3 |
| F-MBX-1 | LPC I/O mailbox (byte FIFOs both ways, 4 ports at 0x0B00) carrying the programmer protocol (07-console-protocol): ROM management, booting, updates | 1 |
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
| E-ROM-2 | Runtime ROM selection: boot config default, PC tool, gamepad hotkey, Xbox mailbox, legacy bank writes, LCD | 1→3 |
| E-ROM-3 | **Loader boot** (06-boot-flow): the boot default is our Loader ROM, which reads the boot reason from the SMC (eject → System, power → the user's bank), arms the target for one boot and resets through the SMC. The chip itself can't see the boot reason (unpowered when the button is pressed; nothing on LPC says why). | 1 |
| E-ROM-4 | **Failed-boot recovery and trial updates** (06-boot-flow §6–7): the ESP32 detects a failed armed boot from LPC activity and resets back to the Loader; Loader and System updates boot once on trial and only become current after confirming | 1 |
| E-USB-1 | USB device stack: **pico_usb** core with a new FPGA DCD; XID and MSC functions from ox; hub emulation | 2 |
| E-BT-1 | Bluetooth gamepads via BlueRetro, mapped to `GamepadIn`/`GamepadOut` | 2 |
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
 LAD[3:0] LCLK LRESET# ─►│ │ lpc_target │────────────►│ spi_stream   │◄──────►│ QSPI NOR 16 MB  │◄─┐ │
 LFRAME# D0            ◄─│ │ (LCLK dom.)│◄────────────│ +prefetch    │ config │ bitstream, boot │  │ │
                         │ └┬──┬──┬──┬──┘             │ +flash_share │  pins  │ cfg, ROM banks  │  │ │
                         │  │  │  │  └─ mem writes ──►│ +flash_emu   │        └─────────────────┘  │ │
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
 USB-UART A ────────────►│ UART0 flash/PC proto VSPI: link only   HSPI: SD + LCD ⇄ config bus*  ├┘
 microSD, SPI LCD ◄─────►│ core 0: BlueRetro BT    core 1: fpga_svc, pico_usb, rom_mgr, legacy,   │
 BT gamepads ))))        │                                 msc_sd, led, host_link, lcd, smbus     │
 NeoPixel ×2 ◄───────────│ RMT                                                                    │
                         └────────────────────────────────────────────────────────────────────────┘
 * HSPI normally runs the SD/LCD pins; it is re-initialised onto the config-bus pins only for
   factory programming / recovery (FPGA held in reset). 03-esp32 §7.1.
```

### 3.1 Key decisions

| # | Decision | Status | Rationale |
|---|---|---|---|
| D1 | **ROMs live in a QSPI NOR on the FPGA's config bus; the FPGA self-boots from it and serves ROMs straight from it.** PSRAM dropped. ESP32 flash holds only firmware (+ a golden bitstream for recovery). | [DECIDED] | The UP5K needs *some* external memory regardless (128 KB SPRAM < smallest 256 KB BIOS), so it's NOR vs. PSRAM, not NOR vs. nothing. NOR: same part count, non-volatile, holds the bitstream too, no ESP32 on the boot path (P-1, §4), flasher emulation is natural. |
| D2 | ESP32 is wired to the config bus too: it can hold `CRESET_B` low and program the NOR directly (factory programming, unbrick), or slave-configure the FPGA after putting the NOR in deep power-down. With the FPGA running, the ESP32 **borrows** the NOR for every read, erase and program: the FPGA floats its pins once the console is quiet and answers BIOS reads with 0xFF meanwhile. There is no flash engine in the FPGA | [DECIDED] | No external programmer ever needed; a bad bitstream can't brick the board. The UP5K can't hold a flash engine as well as the USB device, and ROM writes happen only from the user's menu, which runs from RAM (A/B banks: the old bank keeps being served until the next boot). |
| D3 | ESP32↔FPGA link uses **separate** FPGA pins from the config bus | [PROPOSED] | After boot the FPGA is master of the NOR on the config pins; the ESP32 needs its own always-available slave link. |
| D4 | USB: FPGA PHY+SIE with **multiple device addresses and a pooled EP buffer table**; ESP32 runs **pico_usb** with a new DCD | [PROPOSED] | Only one physical port (MCPX test pads). pico_usb already does composite *and* hub emulation (hub + N downstream devices sharing one wire) and ox's XID/MSC functions are written against it. |
| D5 | LPC on `LCLK` directly; everything else on 48 MHz PLL from a 12 MHz oscillator | [PROPOSED] | USB needs an accurate 48 MHz; LPC timing follows the host clock. |
| D6 | COM: guest divisor stored but ignored, LSR back-pressure, physical UART on its own bridge channel at a fixed programmable baud | [PROPOSED] | No data loss regardless of guest settings; works for kernel debugging without ESP32 involvement. |
| D7 | Legacy modchip compatibility = **generic FPGA I/O trap + ESP32 personalities**, plus two hardware fast-paths (bank select, JEDEC flash) | [PROPOSED] | Keeps gateware generic; new personalities are firmware-only. |
| D8 | SD card on the **ESP32's** second SPI bus (shared with the LCD), raw block access, presented to the Xbox only as USB mass storage | [DECIDED] | The USB stack is on the ESP32, so every MSC block passes through it anyway; this way blocks cross the link once instead of twice. SPI SD (~2 MB/s) exceeds USB FS bulk (~1 MB/s). Saves FPGA LUTs/EBR and 5 FPGA pins; ESP-IDF `sdspi` does the card protocol. Link bus stays dedicated to the FPGA (USB latency). |
| D9 | HDL Verilog (Yosys subset); Yosys + nextpnr-ice40 + icepack; Icarus Verilog testbenches. ESP-IDF pinned to the version BlueRetro requires. | [DECIDED] | |
| D10 | First dev board: **UPduino v3.1** (UP5K-SG48, 4 MB flash on the config pins, 12 MHz osc). RTL stays vendor-neutral outside `boards/` so a fallback FPGA (02-fpga §10.1) stays possible | [DECIDED] | Hardware on hand; same FPGA as the target. |

## 4. Cold boot without standby (P-1)

### 4.1 The problem

Without standby, the board powers up with the console. The MCPX starts fetching from LPC shortly after `LRESET#` deasserts. If nobody answers:
- 1.0–1.5 with `D0` driven by us: D0 is released (FPGA unconfigured = I/O floating) → console boots its TSOP. Wrong ROM, but not a failure.
- 1.0–1.5 with D0 grounded, or 1.6: no BIOS → boot failure (FRAG, or the SMC retries).

### 4.2 Budget

| Path | Ready after power-good | Verdict |
|---|---|---|
| **Alternative**: ESP32 boots → slave-configures FPGA → copies ROM to PSRAM | Default IDF config: a few hundred ms to `app_main` ⇒ **~0.3–0.8 s** total. Tuned (skip image validation on power-on, logs off, QIO 80 MHz): ~50–100 ms to `app_main` + config ~35 ms + copy 15–50 ms ⇒ **~100–200 ms** (estimate) | Too slow for the measured deadline below, and every cold boot is a full reload. |
| **D1**: FPGA self-configures from NOR, reads boot config, serves directly from NOR | POR + config of ~104 KB at the configured SPI master clock ⇒ **~20–70 ms** (estimate) | Fits the deadline with margin. |

**Measured deadline (1.6, §4.4): first BIOS read ≈ 98 ms after the 3.3 V rail rises, earliest seen 78 ms**, and it arrives 0–1 µs after `LRESET#` — no grace period. D1 fits; the tuned-ESP32 path (100–200 ms) does not.

MCU-based chips such as ox (RP2350, XIP flash) manage without a separate ROM chip because the MCU is running within milliseconds. The slow element in this system is the ESP32 boot chain, not the FPGA.

The ESP32 boots in parallel and attaches when ready (USB, BT, legacy personalities). Nothing it does is on the boot-critical path. Legacy I/O writes that happen before it's up are queued in the FPGA event FIFO (SPRAM, 8 K entries); reads are served from the boot-config shadow values.

### 4.3 What standby adds (optional wire)

- ESP32 + BT alive while console is off → controllers stay paired; **power on the console from a BT controller** (also needs a power-button wire — optional header).
- ROM switch + automatic power-cycle; NeoPixel effects while off; ESP32 can rewrite the NOR with the console off.
- Board design: OR-ing (ideal diode) between standby 5 V and console 5 V; firmware detects which is present.

### 4.4 Cold-boot measurements

Measured with the `lpc_probe` design on a USB-powered UPduino: passive inputs on 3.3 V sense, `LRESET#`, `LFRAME#` and `LCLK` through series resistors, timestamping rail-up → `LRESET#` → first START and reporting over UART (`fpga/tools/lpc_probe_view.py`).

**1.6, no other device on the bus, 6 cold boots:**

| | LCLK starts | `LRESET#` released | First LPC cycle |
|---|---|---|---|
| Typical (5 of 6) | 97.05–97.23 ms | 98.19–98.37 ms | +0–1 µs |
| Earliest (1 of 6) | 77.1 ms | 78.26 ms | +1 µs |

- The sequence is identical on every boot: `MEM RD FF000070..77`, then `FF000000..` onwards as sequential single-byte reads; 268,621 memory reads, 1 I/O read and 2 I/O writes in the first 4.2 s.
- Reads complete every ~1.1 µs: the stock flash already inserts SYNC waits and the MCPX accepts them.
- LCLK runs ~1.1 ms before `LRESET#` is released.
- The 78 ms boot (probably a faster rail rise on a quick power re-cycle) sets the budget: **FPGA configuration + boot sequence must finish in < ~70 ms.**

**Serving (`lpc_serve`, 1.6, single-bit SPI flash at LCLK/2):** a full boot of a 256 KB image served 271,019 reads with no SYNC timeouts and no data errors. The longest SYNC wait was 87 LCLK (~2.6 µs), which the MCPX accepted.

Not yet measured: 1.0–1.5 boards; the FPGA's own power → `CDONE` → LPC-ready time.

## 5. Phases

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **0 – Bring-up & measure** | Repo skeletons, FPGA build for UPduino v3.1, ESP32 programs NOR + resets FPGA, FPGA self-boots, link with `ID`/scratch; cold-boot timing measurements (§4.4) | ESP32 writes a bitstream to NOR, FPGA boots it, link loopback 10⁶ transfers error-free; timing numbers recorded |
| **1 – BIOS + COM** | NOR controller + prefetch, LPC target incl. **LFRAME# abort**, boot config, 16550 + UART + Super I/O, mailbox + programmer protocol, ESP32 ROM library in NOR + PC protocol, SMBus master, NeoPixels | 1.6 cold-boots 256 K / 512 K / 1 MB banks with no standby (then 1.0–1.5 via D0); switch at reset; kernel debug output on PC COM port |
| **2 – USB + BT** | Multi-address USB SIE, pico_usb FPGA DCD, XID, hub emulation, BlueRetro | BT controller plays through the test-pad port; composite vs. hub result recorded (Q2) |
| **3 – Legacy + console-side management** | iotrap + bank map + JEDEC flash emulation, first personality (Xenium: bank + LED + LCD), SPI LCD | An existing Xbox flasher writes a ROM into a bank; XeniumOS-style LCD/LED output appears on our LCD/NeoPixels |
| **4 – SD mass storage** | `msc_sd` on the ESP32 (`sdspi` raw blocks) behind the MSC function over the FPGA USB | Xbox reads/writes the SD card (e.g. as an MU/USB drive) and can upload ROMs from it |

## 6. Open questions

| # | Question | Default |
|---|---|---|
| Q2 | Multi-controller on one root port: composite (4 XID interfaces, as ox's default) vs. hub emulation (Duke-like: hub + XIDs + MU). Which does the stock kernel map to players 1–4, and does it accept an MU/MSC device? | Build both (cheap in this design), decide by experiment |
| Q3 | Which legacy chips matter first (Xenium, SmartXX, Xecuter 3, Aladdin…), and which flashers must the JEDEC emulation satisfy (Raincoat, XBlast, EvoX, XeniumOS…)? This determines the ID code(s) to report | Xenium first; report an SST49LF080A-class ID |
| Q4 | 5 V for NeoPixels and the board without standby: controller-port VBUS or the LPC header's 5 V pin? How much current does each supply, and how much 3.3 V is available on the header? | Board powered from controller-port 5 V via its own regulator |
| Q6 | Licence | Undecided. Third-party code rules: [CREDITS.md](CREDITS.md) |
| Q7 | If the UP5K turns out inadequate (LUTs, timing), which fallback FPGA? See 02-fpga §10.1 | Decide on real Yosys numbers; GW1N-9 (no PSRAM) or iCE40HX4K, both keeping the NOR architecture |
