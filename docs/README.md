# OpenOX FPGA — Design Documents

| Doc | What it covers |
|---|---|
| [00-system.md](00-system.md) | Goals, requirements, system architecture, key decisions, phases, open questions |
| [01-interface.md](01-interface.md) | **The contract** between ESP32 and FPGA: configuration sequence, SPI link protocol, register map, IRQs |
| [02-fpga.md](02-fpga.md) | iCE40UP5K gateware: clocks, modules, LPC target, NOR + flash emulation, legacy I/O trap, USB FS multi-device, COM, resources, fallback FPGA options |
| [03-esp32.md](03-esp32.md) | ESP32-PICO-MINI-02 firmware: tasks, ROM management in NOR, legacy personalities, pico_usb + FPGA DCD, SD card as MSC, Bluetooth, LEDs, LCD |
| [04-hardware.md](04-hardware.md) | Pin budgets for both chips, power, Xbox wiring, dev-board bring-up plan, BOM sketch |

Status markers used throughout:

- **[DECIDED]** — settled; change only with a reason.
- **[PROPOSED]** — recommended default; confirm or override.
- **[OPEN]** — needs a decision before the phase that depends on it.
- **[VERIFY]** — a fact taken from memory or a reference that must be checked against the datasheet before hardware is committed.

Reference code lives in `../refs/` (not part of this project):

| Ref | Used for |
|---|---|
| `refs/ox` (RP2350) | LPC decode/responder behaviour, window/mirroring rules, serve-at-reset, 16550 emulation (guest baud ignored, LSR back-pressure), XID/MSC USB functions |
| `refs/ox/thirdparty/pico_usb` (Apache-2.0) | USB device core for the ESP32 side: pluggable DCD, composite and hub emulation with multiple downstream devices |
| `refs/EOS` (Gowin FPGA) | RTL LPC target on LCLK, SYNC timeout handling, 1.6 LFRAME# abort, D0 handling, serve-while-filling, status LED ideas |
| `refs/xprobe` | SMSC-style Super I/O config space (0x2E/0x2F) so debug kernels find the COM port |
| `refs/lpc-uart` | Minimal RTL LPC → UART device |
| `refs/valentyusb` (BSD-3) | iCE40UP5K-proven USB FS PHY/SIE; `eptri` is the model for the CPU-driven endpoint interface |
| `refs/FPGA-USB-Device` (LGPL-2.1) | Readable Verilog USB FS bit/packet layers (needs 60 MHz; licence is copyleft — study, don't paste) |
| `refs/Gowin_USB1.1_SoftPHY_RefDesign` | Alternative soft PHY (vendor licence — reference only) |
| `refs/oxblueretro` (Apache-2.0) | BlueRetro Bluetooth stack on ESP32-PICO-MINI, `components/gamepad` `GamepadIn`/`GamepadOut` model |
