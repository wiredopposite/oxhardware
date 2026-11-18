# oxctl

`tools/oxctl.py` is the PC tool for the device. It manages the ROM library, presses the console's buttons, talks to its SMC, updates the device and runs the kernel debugger.

It needs Python 3 and `pyserial` (`pip install pyserial`).

## Ports

The device shows up on the PC as two USB serial ports:

| Port | What it's for | Option |
|---|---|---|
| The ESP32's | Every command except the kernel debugger's | `-p COMx`, or set `OXCTL_PORT` |
| The console's COM1 | Only the `kd` commands | `--com COMy`, or set `OXKD_COM` |

Setting both variables saves typing them each time. The examples below leave `-p` out. In PowerShell:

```
$env:OXCTL_PORT = "COMx"
python tools/oxctl.py rom list
```

## Common tasks

Add a ROM and make it the one the power button boots:

```
python tools/oxctl.py xbox off
python tools/oxctl.py rom upload mybios.bin --user
python tools/oxctl.py xbox power
```

Boot a ROM once, without changing the power-button ROM:

```
python tools/oxctl.py xbox off
python tools/oxctl.py rom upload test.bin --boot
python tools/oxctl.py xbox power
```

The boot after that goes back to the Loader as usual. To boot a ROM that's already in the library once, use `rom boot ID`.

Check the device is answering:

```
python tools/oxctl.py sys status
```

**Turn the console off before uploading a ROM.** The console reads its BIOS from the same flash, and an upload made while it's on fails its CRC check. Uploads from OpenOX's boot menu are the exception, since the console runs from RAM there.

## Options

These go before the command group, e.g. `oxctl.py -v rom list`.

| Option | |
|---|---|
| `-p`, `--port` | The ESP32's port. Defaults to `OXCTL_PORT`. |
| `-b`, `--baud` | 921600 by default. |
| `-v`, `--verbose` | Also prints the ESP32's log and events while the command runs. |

Addresses, lengths, IDs and SMBus values take decimal or `0x` hex.

## ROMs

See [ROMs](roms.md) for the library itself: the roles, slots and what boots when. `rom list` shows each entry's ID, which the other commands take.

| Command | What it does |
|---|---|
| `rom list` | Lists every entry: ID, role, slot, size, CRC, name and flags. The flags are `current` (the Loader, System or Diag in use), `user` (the power-button ROM), `trial` and `failed-boot`. |
| `rom upload FILE` | Adds a ROM to the library. Options below. |
| `rom download ID OUT` | Saves an entry to the file `OUT`. `rom download active OUT` saves the ROM the console is running now. |
| `rom delete ID` | Deletes an entry. |
| `rom rename ID NAME` | Renames an entry, up to 32 characters. |
| `rom user ID` | Makes the entry the power-button ROM. `rom user none` clears it, and the power button then boots System. |
| `rom boot ID` | Boots the entry once, from the console's next power-on or reset. With `--reset`, it resets the console now to boot it straight away. |
| `rom compact SIZE` | Moves entries until there's a free run of `SIZE` (`256K`, `512K` or `1M`). |

`rom upload` options:

| Option | |
|---|---|
| `--name NAME` | The entry's name. Defaults to the file name without its extension. |
| `--type user\|loader\|system\|diag` | Its role. `user` by default. |
| `--user` | Also make it the power-button ROM. |
| `--boot` | Also boot it once, from the next power-on or reset. |
| `--trial` | For a Loader or System update: it boots once on trial and only replaces the old one after it confirms. See [Boot flow](../dev/boot-flow.md#trial-updates). |
| `--compact` | If there's room in total but no free run the right size, moves entries to make one. Without it, the upload stops and says so. |
| `--reset` | Resets the console through the SMC once the upload is done. |

## The console

| Command | What it does |
|---|---|
| `xbox state` | `off`, `reset` or `running`. |
| `xbox power` | Presses the power button. |
| `xbox eject` | Presses the eject button. Pressing it to turn the console on boots System. |
| `xbox reset` | Resets the console through the SMC. This works even when it's hung. |
| `xbox cycle` | Power-cycles it through the SMC. |
| `xbox off` | Turns it off through the SMC. |
| `xbox status` | The tray state, AV pack and the CPU and board temperatures, read from the SMC. |
| `smbus read ADDR REG [--word]` | Reads a byte, or a word with `--word`, from the console's SMBus. E.g. `smbus read 0x10 0x03` reads the SMC's tray state. |
| `smbus write ADDR REG VALUE [--word]` | Writes a byte or a word. |

## Kernel debugger

These commands drive a KD build of OpenOX's BIOS through the console's COM1. Every `kd` command needs the COM port (`--com`). `kd debug` also needs the ESP32's port, since it uploads the BIOS and turns the console on.

| Command | What it does |
|---|---|
| `kd debug BIOS` | The whole loop, in one command. 1. Turns the console off if it's on. 2. Deletes the user ROMs named `kd` and uploads `BIOS` in their place, to boot once. The power-button ROM stays as it was. 3. Presses power and waits for the kernel's boot break. If it doesn't come, boots the ROM again with a reset. 4. Bridges COM1 to TCP for lldb until Ctrl+C. |
| `kd bridge` | Only step 4, for a console that's already running a KD build. |
| `kd probe` | Asks the kernel why it's stopped, and prints the answer. |
| `kd kill` | For a console stopped in the debugger: the kernel resets it through the SMC. |

Then in lldb: `gdb-remote 2345`.

| Option | |
|---|---|
| `--com COMy` | The COM1 port. Defaults to `OXKD_COM`. |
| `--tcp PORT` | The TCP port for lldb, 2345 by default. |
| `--name NAME` | The ROM name `kd debug` replaces, `kd` by default. Don't give a ROM you want to keep this name. |
| `--wait SECONDS` | How long `kd debug` and `kd probe` wait for the kernel, 20 by default. |
| `--traffic` | Prints the debugger traffic. |
| `--com-baud` | 115200 by default. The device's COM1 always runs at 115200, whatever the console programs. |

The bridge takes one debugger at a time and drops the kernel's output while none is connected. When the kernel resets the console itself, lldb stays connected and stops at the next boot break. OpenOX's side of this, VS Code included, is in OpenOX's `docs/dev/debugging.md`.

## Updates

These work with the console running.

| Command | What it does |
|---|---|
| `update fpga FILE` | Writes a new FPGA bitstream and reloads the FPGA, which takes a few seconds. |
| `update esp32 FILE` | Writes new ESP32 firmware. The ESP32 restarts once it's written. |
| `tables upload FILE [--source CRC]` | Uploads the retail tables the System BIOS is built from. `CRC` is the CRC32 of the retail BIOS they came from, recorded with them. |
| `tables download OUT` | Saves the stored retail tables to a file. |

## Controller and Bluetooth

The device is the console's controller port 1: a USB gamepad behind a hub, fed by Bluetooth controllers through BlueRetro.

| Command | What it does |
|---|---|
| `bt status` | Whether pairing is on, how many controllers are connected and whether the configuration page is open. |
| `bt pair on\|off` | Starts or stops pairing. It also starts on its own at startup and whenever the last controller disconnects. |
| `bt config on\|off` | Opens or closes BlueRetro's BLE configuration page. It has no password, so it's closed at startup. |
| `bt forget` | Disconnects every controller and clears all pairing keys. |
| `usb status` | The USB state of the gamepad and hub, and transfer counters. |
| `usb stop`, `usb start` | Detaches the whole USB device from the console, and attaches it again. A stop lasts until `usb start` or the ESP32 restarts. |
| `usb unplug`, `usb plug` | Unplugs the gamepad from the hub, and plugs it back in. |
| `usb pad [BUTTONS...] [--lx N] [--ly N] [--rx N] [--ry N]` | Sets what the gamepad reports, until the next `usb pad`: the buttons named are held, and the sticks run from -32768 to 32767. E.g. `usb pad a start --lx 20000`. `usb pad` alone releases everything. It prints the last rumble the console sent. |

The `usb pad` buttons are `up` `down` `left` `right` `start` `back` `ls` `rs` `a` `b` `x` `y` `black` `white` `lt` `rt`.

## The ESP32

| Command | What it does |
|---|---|
| `sys ping` | Checks the link answers. |
| `sys version` | The firmware and ESP-IDF versions, and when it was built. |
| `sys status` | Uptime, free heap (now, the lowest it's been and the largest free block) and why it last restarted. |
| `sys reboot` | Restarts the ESP32. |
| `sys heap` | Free heap now, a sample every 5 minutes for the last 8 hours, any allocation that failed, and every task with its stack headroom. |
| `sys trace start [N]` | Starts recording allocations that aren't freed, up to `N` (64 by default). |
| `sys trace dump` | Lists the recorded allocations that are still held, with who made them. |
| `sys trace stop` | Stops recording. |
| `log level LEVEL [TAG]` | Sets the log level, for one tag or all of them: `none`, `error`, `warn`, `info`, `debug` or `verbose`. |
| `monitor [--reset]` | Prints the log and events until Ctrl+C. `--reset` restarts the ESP32 first, to see it start up. |

`sys trace dump` names the functions from the firmware's ELF, `esp32/build/openox.elf` by default (`--elf` for another), through ESP-IDF's `xtensa-esp32-elf-addr2line`. Without them it prints addresses.

## Bring-up and diagnostics

These are for working on the device itself.

**FPGA and its flash.** `fpga id`, `read` and `write` hold the FPGA in reset while they run and release it at the end. Meanwhile the console has no BIOS. `iceprog` mustn't be running.

| Command | What it does |
|---|---|
| `fpga status` | Whether the FPGA is configured (CDONE), and whether the ESP32 is holding it in reset. |
| `fpga id` | The FPGA flash's JEDEC ID. |
| `fpga probe` | Checks the wiring to the FPGA's flash. |
| `fpga read OUT --addr A --len N` | Reads the FPGA's flash to a file. |
| `fpga write FILE [--addr A]` | Erases, writes and verifies the FPGA's flash at a 4KB-aligned address, 0 by default. |
| `fpga hold`, `fpga release` | Holds the FPGA in reset until `fpga release`, which lets it configure. |

**The SPI link to the FPGA.** `--mhz F` on any `link` command sets the link clock first. It's kept until the ESP32 restarts.

| Command | What it does |
|---|---|
| `link info` | The FPGA design's version, board and features, and the LPC reset state. |
| `link probe` | Checks the link wiring. |
| `link test [--count N]` | Writes and reads back the scratch register `N` times, 200 by default. |
| `link read ADDR [--len N] [--space S]` | Reads FPGA registers, see [Link](../dev/link.md#registers). `S` is 0 registers (default), 1 USB slots, 2 ROM flash reads, 3 FIFOs. |
| `link write ADDR HEXBYTES [--space S]` | Writes them, e.g. `link write 0x0C 01020304`. |
| `link irq` | How many FPGA interrupts the ESP32 has taken, by bit. |

**The ROM flash**, through the running FPGA. `nor write` and `nor erase` bypass the library, so they can break it.

| Command | What it does |
|---|---|
| `nor info` | Whether the flash answers, and its ID. |
| `nor read OUT --addr A --len N` | Reads it to a file. |
| `nor write FILE --addr A` | Erases, writes and verifies, at a 4KB-aligned address. |
| `nor erase --addr A --len N` | Erases. |
| `nor crc --addr A --len N` | The CRC32 of a range. |

**The [programmer protocol](../dev/programmer-protocol.md)**, the one the console's boot menu uses.

| Command | What it does |
|---|---|
| `prog info` | The protocol and firmware versions, the console's state, the library's current entries and its free slots. |
| `prog smbus-read ADDR REG`, `prog smbus-write ADDR REG VALUE` | SMBus through the programmer. |
| `prog confirm` | Confirms the trial boot that's running now. |
