# Architecture

## Two chips

| Chip | Does |
|---|---|
| iCE40UP5K FPGA | Everything with hard timing: the LPC target (BIOS reads, the COM port and Super I/O, the boot control port, the programmer's mailbox), the USB full-speed device, reading ROMs from its flash |
| ESP32-PICO-MINI-02 | Everything with policy, protocols or radios: the ROM library, deciding and checking boots, updates, the PC connection, the USB device stack, Bluetooth gamepads, the console's SMBus and buttons |

```
 LPC header ──────► FPGA ◄───── config bus ─────► flash (bitstream, ROMs)
 (LAD, LCLK,         │  │                              ▲
  LRESET#, LFRAME#)  │  └─ COM UART ──► USB serial     │ borrowed for
                     │                                 │ every write
 controller port ◄───┤ USB                             │
                     │                                 │
                     └── SPI link + IRQ ──► ESP32 ─────┘
                                             │
                     SMBus, power/eject ◄────┤
                     PC (oxctl) ◄── UART0 ───┤
                     Bluetooth ))) ──────────┘
```

## The FPGA serves the BIOS on its own

Without standby power, the device powers up with the console, and the MCPX starts fetching its BIOS within 100ms. On a 1.6 the first read comes about 98ms after the 3.3V rail rises, and the earliest seen was 78ms (see [Measurements](measurements.md#cold-boot-timing)). Nothing lets it wait: the first read arrives within a microsecond of `LRESET#` going high.

The ESP32 can't be on that path. It takes a few hundred milliseconds to reach `app_main`, and around 100ms even tuned. So the FPGA configures itself from its own flash (20 to 70ms) and serves ROMs straight out of that flash. The ESP32 boots alongside and attaches when it's ready, and has to cope with finding the console off, booting or running.

The iCE40 needs external memory for a BIOS anyway (its 128KB of SPRAM is less than the smallest image), so the one flash holds the bitstream and the ROM library.

## The ESP32 owns the flash for writes

The FPGA only ever reads the flash. Every erase and program is the ESP32's, over the config bus (the FPGA's configuration pins, wired to the ESP32 as well):

- **FPGA running:** the ESP32 borrows the flash. The FPGA lets go of it once the console has gone 1ms without a BIOS read, and answers BIOS reads with 0xFF until it's given back. ROM writes from the console happen while the console runs from RAM, so it doesn't notice.
- **FPGA in reset:** the ESP32 holds `CRESET_B` low and has the flash to itself, for the bitstream area and for recovery.

A flash engine in the FPGA would take about 970 LUTs, which don't fit next to the USB device. And with the ESP32 able to rewrite the bitstream at any time, a bad one can't brick the board: the ESP32 keeps the last good one in its own flash.

The ESP32 talks to the running FPGA over a separate SPI link with an interrupt line. See [Link](link.md).

## USB

The console has one USB port the device can reach, its controller port. The FPGA's USB core answers for up to eight devices at different addresses on that one port, so the ESP32 can present a hub with devices behind it, as a real controller does. The FPGA does every handshake from its slot tables. The ESP32 runs the USB device stack (a port of pico_usb) with a driver for the FPGA, and only moves packets in and out of slots.

## Clocks

| Clock | Source | Runs |
|---|---|---|
| LCLK | The console, 33.33MHz | The LPC target and the registers the console sees. Stops while the console is off |
| 48MHz | PLL from the 12MHz oscillator | The USB line side |
| 24MHz | Divided from the 48MHz | Everything else: the link back end, banks, flash reads, USB engine, UART |
| Link SCK | The ESP32, 20MHz at most | The link's shift registers |

## Tools

| Part | Tools |
|---|---|
| Gateware | Verilog, Yosys, nextpnr-ice40 and icepack from OSS CAD Suite. Icarus Verilog testbenches |
| Firmware | ESP-IDF, C++ for the project's components. BlueRetro's Bluetooth stack stays in C |
| PC | Python 3 with pyserial |

RTL outside `fpga/boards/` stays vendor-neutral, so a different FPGA would need new board files rather than a rewrite.
