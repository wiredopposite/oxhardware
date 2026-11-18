# Status

What works today is on the bench: a UPduino v3.1 and an ESP32-PICO-DevKitM-2 on a 1.6. This page is what's planned and not built yet.

## Working

- [x] Serving 256KB to 1MB BIOS images on a 1.6, with the `LFRAME#` abort
- [x] The ROM library, the Loader's boot flow, failed-boot detection and trial updates
- [x] COM port and Super I/O: the devkit kernel debugger works over the UART
- [x] The programmer protocol from the console and the PC
- [x] USB: a hub and an Xbox gamepad on the controller port, driven by Bluetooth controllers
- [x] SMBus, the console's buttons and power state
- [x] Updating the bitstream and firmware with the console running
- [x] Long uptimes: the heap holds steady through Bluetooth reconnects, console power cycles, uploads and USB unplugs (`oxctl sys heap`, `sys trace`)

## Gateware

- [ ] **1.0 to 1.5:** drive `D0` low (open-drain) while serving, and release it to fall back to the TSOP, including during `LRESET#`. An unconfigured FPGA leaves it floating, so a failed boot falls back to the TSOP.
- [ ] **Boot config record:** a record in flash the FPGA reads at power-up, so the default, user, System and Diag banks, the protected slots and the port bases are right before the ESP32 is up. Today they start from build parameters, and the Loader sees no user or System bank until the ESP32 sets them.
- [ ] **Faster flash reads:** quad I/O reads (`0xEB`) with continuous mode and a 32-byte prefetch line, on a board whose flash has IO2 and IO3 wired. The UPduino's doesn't.
- [ ] **Slot protection in the FPGA:** a `PROTECT_MASK` of slots that console-side flashing can't touch. Today `rom_mgr` and the programmer enforce it.
- [ ] **COM options:** a programmable baud (`UART_DIV`), RTS/CTS, and routing COM to the ESP32 instead of the UART pins.
- [ ] **Status registers:** SYNC timeout count and setting, `ERR_STATUS`, `SOFT_RESET`, a register-controlled LED.

## Compatibility with other LPC devices

So that existing Xbox software that drives a Xenium, SmartXX, Xecuter 3 or Aladdin works with this device.

- [ ] **I/O trap** in the FPGA: eight programmable I/O port windows. Writes are captured in order into a deep FIFO (SPRAM, 8K entries) for the ESP32, with a coarse timestamp. Reads come from shadow registers the ESP32 keeps, or are deferred to the ESP32. Writes that happen before the ESP32 is up wait in the FIFO.
- [ ] **Bank-select fast path:** a written value maps through a table straight to the active or pending bank, before the ESP32 is up. XeniumOS switches banks at runtime, so it has to apply at once as well as at reset (see [Measurements](measurements.md#a-xenium-on-the-same-bus)).
- [ ] **JEDEC flash emulation:** answers AMD/SST-style command sequences on memory writes to the BIOS window (autoselect ID, byte program, sector and chip erase, DQ7 and DQ6 status polling), so existing flashers can write a ROM into the active bank. Chip erase only touches the active bank, and protected slots are refused.
- [ ] **Personalities** on the ESP32: a table of trap windows and a decoder per device, turning trapped traffic (HD44780 commands, bit-banged SPI, LED registers) into LCD text, LED colours and bank selects. Xenium first.

The I/O trap and flash emulation would take about 650 LUTs together, which don't fit beside the USB device on the UP5K (`lpc_com` already uses 84%).

## ESP32

- [ ] **Memory units:** MSC devices on hub ports 2 and 3, backed by an SD card on the ESP32's second SPI bus as raw blocks. The ESP32 never mounts the card while the console has it.
- [ ] **NeoPixels and an SPI LCD:** device status, the active ROM, controllers, and a ROM picker driven by the gamepad. The LCD shares the SD card's bus.
- [ ] **A gamepad hotkey** to choose the ROM.
- [ ] **Standby power:** with standby 5V, the ESP32 and Bluetooth stay up while the console is off, controllers stay paired and can turn the console on, and ROMs can be switched with the console off.

## Custom board

Needed for everything the UPduino can't do: 1.0 to 1.5 (`D0`), quad flash, the SD card and LEDs.

### FPGA pins (iCE40UP5K-SG48, 39 I/O)

| Group | Signals | Count |
|---|---|---|
| LPC | LAD0 to 3, LCLK (on a global-buffer input), LRESET#, LFRAME# gate and sense, D0 | 8 |
| Flash | SCK, SS, SO, SI (the fixed configuration pins), IO2, IO3 | 6 |
| ESP32 link | SCK, MOSI, MISO, CS, IRQ | 5 |
| Clock | 12MHz in, on a global-buffer input | 1 |
| USB | D+, D-, pull-up, adjacent and in one bank | 3 |
| COM UART | TX, RX, RTS, CTS | 4 |
| RGB LED | The dedicated current-sink pins | 3 |
| Debug | Two spare outputs for a logic analyser | 2 |
| **Total** | | **32** |

`CRESET_B` and `CDONE` are dedicated pins. Pin 35 can't be used while the PLL runs.

### ESP32 pins (ESP32-PICO-MINI-02)

| Group | Signals | Count | Notes |
|---|---|---|---|
| Programming | U0TXD, U0RXD, EN, GPIO0 | 4 | DTR/RTS auto-reset |
| Link | SCK, MOSI, MISO, CS | 4 | Through the GPIO matrix, since the module has no GPIO16, 17, 18 or 23 |
| SD and LCD | SCK, MOSI, MISO, SD CS, LCD CS, LCD DC, LCD backlight | 7 | Not GPIO2, 12 or 15. 10kΩ pull-up on SD MISO |
| FPGA control | CRESET, CDONE, IRQ | 3 | CDONE and IRQ on input-only pins |
| Config bus | SCK, MOSI, MISO, SS | 4 | Separate wires from the SD bus, 33Ω in series. Not GPIO12: an unconfigured iCE40 pulls its pins high, and GPIO12 high at reset selects 1.8V flash |
| NeoPixel | Two data lines | 2 | Through 74AHCT1G125 to 5V |
| SMBus | SDA, SCL | 2 | The console's pull-ups |
| Standby | Power button, eject button, standby detect | 3 | Optional |

### Power

- 5V from the console, from the controller port next to D+ and D- or LPC header pin 6, with optional standby 5V on its own connector, OR-ed through an ideal diode.
- 3.3V from its own regulator, 800mA or more (the ESP32's radio peaks around 500mA). It has to come up fast: the FPGA must configure before the MCPX leaves reset.
- 1.2V for the iCE40 core and PLL. VPP tied to 3.3V.
- NeoPixel 5V through a polyfuse, with a firmware current limit.
- The iCE40's pins aren't 5V tolerant. Series resistors on LAD, LCLK and LRESET#.

### Parts

| Part | Example |
|---|---|
| FPGA | iCE40UP5K-SG48I |
| MCU | ESP32-PICO-MINI-02-N8R2 |
| Flash | W25Q128JV (16MB, quad, 4KB sectors) |
| USB serial | CH342F or CP2105: one channel to the ESP32, one to the FPGA's COM |
| Oscillator | 12MHz, 3.3V CMOS |
| `LFRAME#` switch | IRLML6244 |
| Protection | USBLC6-2, polyfuse, ideal diode for standby |

## Open questions

- **One controller port, several devices:** a hub with devices behind it, as now, or a composite device? Which does the stock kernel map to players 1 to 4, and does it take a memory unit behind the hub?
- **Power:** how much current the controller port and the LPC header's 5V can each supply, and how much 3.3V is on the header.
- **Licence:** undecided. Third-party code rules are in [Credits](../credits.md).
- **Not enough LUTs:** if the UP5K runs out, an iCE40HX4K (whose die the open toolchain can use as 7680 LUTs, same tools, but no SPRAM or RGB driver) or a GW1N-9 (8640 LUTs, internal flash, a different toolchain). Both keep the ROMs in an external flash.

## Docs

- [ ] Wiring for 1.0 to 1.5, once `D0` is in
