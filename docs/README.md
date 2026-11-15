# OpenOX FPGA — Design Documents

| Doc | What it covers |
|---|---|
| [00-system.md](00-system.md) | Goals, requirements, system architecture, key decisions, phases, open questions |
| [01-interface.md](01-interface.md) | **The contract** between ESP32 and FPGA: configuration sequence, SPI link protocol, register map, IRQs |
| [02-fpga.md](02-fpga.md) | iCE40UP5K gateware: clocks, modules, LPC target, NOR + flash emulation, legacy I/O trap, USB FS multi-device, COM, resources, fallback FPGA options |
| [03-esp32.md](03-esp32.md) | ESP32-PICO-MINI-02 firmware: tasks, ROM management in NOR, legacy personalities, pico_usb + FPGA DCD, SD card as MSC, Bluetooth, LEDs, LCD |
| [04-hardware.md](04-hardware.md) | Pin budgets for both chips, power, Xbox wiring, dev-board bring-up plan, BOM sketch |
| [05-devboard-wiring.md](05-devboard-wiring.md) | Bench wiring: UPduino v3.1 + ESP32-PICO-DevKitM-2 + Xbox |
| [06-boot-flow.md](06-boot-flow.md) | ROM roles (Loader, System, Diag, user), one-shot armed boots, boot control port, failed-boot detection, trial updates, forced boots |
| [07-console-protocol.md](07-console-protocol.md) | Programmer protocol: console port at 0x0B00, packets, commands, the shared `libs/oxprog` library |
| [08-oxctl.md](08-oxctl.md) | The PC tool: ROMs, the console's power and SMC, the kernel debugger, chip updates, controller, Bluetooth, diagnostics |

Status markers used throughout:

- **[DECIDED]** — settled; change only with a reason.
- **[PROPOSED]** — recommended default; confirm or override.
- **[OPEN]** — needs a decision before the phase that depends on it.

Credits and provenance: [CREDITS.md](CREDITS.md).
