# ESP32

The firmware for the ESP32-PICO-MINI-02, in `esp32/`: an ESP-IDF project with one component per job. Building it is in [Building](building.md#esp32).

The classic ESP32 is used for Bluetooth Classic, which BlueRetro needs for most controllers. It has no USB of its own: all USB goes through the FPGA.

The ESP32 isn't on the cold-boot path. The FPGA serves the BIOS on its own (see [Architecture](architecture.md#the-fpga-serves-the-bios-on-its-own)), and the ESP32 attaches when it's ready, whether the console is off, booting or running.

## Components

| Component | What it does |
|---|---|
| `host_link` | The [PC protocol](#pc-protocol) on UART0 |
| `cfg_bus` | The config bus: holding the FPGA in reset, and the flash commands |
| `fpga_link` | The SPI link to the FPGA |
| `fpga_svc` | The FPGA's interrupt line, and waking the tasks that asked for each interrupt |
| `nor` | The flash through the running FPGA, borrowed for each operation |
| `chip_update` | Updates to the FPGA's bitstream and the ESP32's firmware |
| `rom_mgr` | The [ROM library](#rom-library), allocation, armed boots, failed-boot detection and trials |
| `mailbox` | The programmer protocol's server, for the console's mailbox and the PC |
| `smbus` | The console's SMBus |
| `console_power` | The console's power state, and its power and eject buttons |
| `usbd` | pico_usb's device stack, with the FPGA's driver `dcd_fpga` |
| `usb_dev` | The USB task, and the device the console sees: a hub with a gamepad |
| `blueretro` | The libblueretro submodule: BlueRetro's Bluetooth host, controller drivers and mapping |
| `bt_xid` | BlueRetro's output for the Xbox gamepad |
| `board` | The pin map |

`main` starts them in order: the PC link, the config bus, the FPGA link and its interrupts, console power, the flash, chip updates (which write the last good bitstream back if the FPGA doesn't configure), SMBus, the ROM library, the mailbox, Bluetooth, then USB.

## Flash layout

The ESP32's own flash. The layout fits 4MB, so it works on a 4MB module and on the devkit's 8MB.

| Name | Type | Offset | Size | Contents |
|---|---|---|---|---|
| `nvs` | data/nvs | `0x009000` | 24KB | ESP32 settings |
| `otadata` | data/ota | `0x00F000` | 8KB | |
| `phy_init` | data/phy | `0x011000` | 4KB | |
| `ota_0` | app | `0x020000` | 1.5MB | Firmware |
| `ota_1` | app | `0x1A0000` | 1.5MB | Firmware |
| `golden` | data/0x40 | `0x320000` | 256KB | Two 128KB bitstream slots: the last good one, and the one being uploaded |
| `storage` | data/spiffs | `0x360000` | 640KB | BlueRetro's settings and pairing keys |

## Chip updates

The bitstream and the firmware are uploaded through the programmer protocol like ROMs (upload types `FpgaBitstream` and `Esp32Firmware`), from the console or with `oxctl update`. The console keeps running.

- **Bitstream:** staged in `golden`'s upload slot (a 4KB header written last, then the image), and checked for its CRC and the iCE40 sync word. It's then written to the FPGA's flash with the FPGA running, and the FPGA is reloaded, which takes about 70ms. USB unplugs and comes back. If the new design doesn't answer on the link, the last good bitstream is written back. Once it answers, it becomes the last good one.
- **Firmware:** written to the OTA partition that isn't running, checked (ESP-IDF's image check and the upload CRC) and set as the boot partition. The ESP32 restarts after its reply. The new firmware is on trial: it confirms itself after 30 seconds, and if it resets before then the bootloader goes back to the previous one.

A release sends the bitstream before the firmware, since the firmware restarts the ESP32.

## ROM library

`rom_mgr` keeps the library in the FPGA's flash (the layout is in [Link](link.md#flash-layout)).

**Catalogue:** two 4KB copies. Each write goes to the other copy with the sequence number plus one, and is verified by CRC. The newest valid copy wins, and a copy that can't be read is never treated as empty. Little-endian and packed:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `'X','O','R','C'` |
| 4 | 2 | Version (1) |
| 6 | 2 | Entry size (52) |
| 8 | 4 | Sequence (wraps) |
| 12 | 1 | Entry count (64) |
| 13 | 3 | Reserved |
| 16 | 52 × 64 | Entries |
| 3344 | 4 | CRC-32 of everything before it |

An entry is: role (1, the protocol's `Role`), bank (1, the `ACTIVE_BANK` format, `0xFF` unused), flags (1, `RomFlags`), reserved (1), size (4), CRC-32 (4), source CRC-32 (4), version (2), reserved (2) and name (32, NUL-padded). An entry's ID is its index, so IDs don't change when others are deleted.

**Retail tables:** the data tables OpenOX's System BIOS is built from, taken from the user's own retail BIOS by the host and kept for the console. A 32KB area after the catalogue: a header sector, then up to 28KB of tables. The header is written last: magic `'X','T','B','L'`, size, CRC-32, source CRC-32 (of the BIOS they came from) and a check word. There's one copy, and an upload erases the header first, so one cut off leaves no tables until it's repeated.

**Allocation:** an upload takes a free run of slots aligned to its size (256KB, 512KB or 1MB). Slots in the catalogue, the active, default and armed banks are never used. Of the free runs it picks the one whose 512KB and 1MB blocks are most used, so small images pack together and 1MB runs stay open.

**Mirrors:** an image that repeats its first 256KB or 512KB is stored as that, and its other slots stay free. The console reads the same bytes, since the bank mirrors through the 1MB window. The entry's size and CRC are those of the stored image.

**Compaction:** when no aligned run is free but moving entries would make one, an upload fails with `Fragmented` instead of `NoSpace`. `Compact` then empties the aligned run that needs the fewest slots moved, one entry per request, largest first. Entries in the active, default and armed banks are never moved. Each move copies the entry, checks its CRC, then saves the catalogue, so a power loss before the save leaves the old entry in use.

**Boots:** after every catalogue change the ESP32 sets the FPGA's user, System and Diag banks from it, and the default bank when there's a current Loader. A watcher task judges every armed boot and follows trial updates, as [Boot flow](boot-flow.md#failed-boots) describes. The `LRESET#` and boot control interrupts wake it, and it polls every 50ms while a boot is being timed.

## FPGA access

**`cfg_bus`** drives the config bus from one of the ESP32's SPI hosts. Holding the FPGA (`CRESET_B` low) lets it program the flash directly, for the bitstream area and recovery. Holding it takes the BIOS off the bus, so it's never done while the console runs without the user asking.

**`nor`** is the flash with the FPGA running. Each operation asks the FPGA for the flash, waits for the grant (up to 3 seconds), takes the bus, runs, waits until the flash is idle, gives the bus back and logs `FLASH_MISS` if the console read meanwhile. Erases and programs below the catalogue are refused unless the FPGA is held.

**`fpga_link`** runs register transactions with polling and bulk ones with DMA, behind one mutex. The bus is the FPGA's alone, so USB never waits behind anything else.

**`fpga_svc`** handles `FPGA_IRQ_N`:

- Tasks register the interrupt bits they want and get a task notification when any of them is raised. The bits only say that something happened, and the task then reads what it needs. `IRQ_ENABLE` is the union of what's registered.
- `IRQ_STATUS` and `IRQ_ENABLE` are read in one burst, and the pending bits are cleared before anyone is notified, so an event after that raises the line again and is never lost.
- The pin interrupt is level-triggered and masks itself, and the task unmasks it after each pass. If the line is low with nothing enabled pending (the FPGA not configured), it stays masked for 100ms.
- The task also runs every 100ms and rewrites `IRQ_ENABLE` if the FPGA has lost it, after a reload.
- `oxctl link irq` shows the interrupts taken, spurious wakeups and counts per bit.

## USB

**`dcd_fpga`** is pico_usb's controller driver for the FPGA's USB core (see [Link](link.md#usb)). The FPGA does every handshake from slot state, and the driver fills IN slots, empties OUT slots and keeps the address and endpoint maps.

- `init` clears the endpoint map and the event registers and turns the SIE on. `connect` turns the pull-up on, and `deinit` turns both off, which the host sees as a detach.
- Device *d*'s EP0 is IN slot *d* and OUT slot 16 + *d*. `ep_open` gives other endpoints a free slot from 8 to 15 or 24 to 31 and an endpoint map entry.
- `ep_write` copies into the IN slot and arms it. `ep_read` reads the length and data, then frees the slot.
- After a bus reset, device 0 (the hub) answers at address 0. A hub port reset moves address 0 to the device behind it. Reading a SET_ADDRESS SETUP arms the FPGA's deferred SET_ADDRESS, so the new address is mapped exactly when the host acknowledges the status stage.
- `dcd_fpga_poll()` reads the USB event registers in one burst, clears exactly the bits it read and queues events per device. An EP0 completion read together with a new SETUP for the same device belongs to the previous transfer, so it goes to usbd first and the SETUP on the next pass.

**`usb_dev`** is one task, woken by the FPGA's USB interrupt and every 2ms. It presents what the console sees as a controller: a 3-port hub (1209:0002) with an Xbox gamepad (1209:0003) on port 1. Ports 2 and 3 are for memory units, which aren't built yet.

- Attached from boot, once the FPGA reports USB. Once a second it checks that the FPGA's USB is still on and attaches again after a reload. Before the ESP32 resets the FPGA it detaches and waits 200ms, so the console sees the unplug before the FPGA's pins float.
- **Gamepad** (`xid_pad.c`): interface class `0x58`/`0x42`, interrupt IN and OUT, 32 bytes, interval 4. It answers the XID descriptor, GET_CAPABILITIES, GET_REPORT (the 20-byte input report) and SET_REPORT or OUT reports (the 6-byte rumble). An input report goes out when the state changes.
- **Hub** (`hub.c`): per-port power switching, all ports removable. A port shows a connection only while it's powered and has a device, and only 100ms after the port was powered or the device attached, as a real hub does: reported at once, the console's hub driver can take the change while it's still powering the other ports and then never asks about that port again. Changes go out on the status change endpoint on every poll until the host clears them, so a report the console drops is sent again. Detaching takes the device off the bus at once, and it answers again only after the host resets its port.
- `oxctl usb` controls it: status, stop and start, unplugging the pad, and setting its buttons and sticks.

## Bluetooth

- **`blueretro`:** BlueRetro's Bluetooth host, controller drivers and mapping as an ESP-IDF component. Its tasks run on core 0, its settings and pairing keys are in `storage`, and its log goes out under the tag `bt`. The Bluetooth name is `BlueRetro_Xbox_XXXX`.
- **`bt_xid`:** the first controller to connect becomes the console's pad. BlueRetro's state becomes the XID input report, the console's rumble goes back to the controller, and the pad is plugged into hub port 1 while a controller is connected and unplugged when it goes. If it goes while another is connected, that one takes over.
- **Home + Start** held for 4 seconds on any controller presses the console's power button.
- BlueRetro's own button combos (L2 + R2 + Start with another button: pairing, disconnect, factory reset) work as in BlueRetro. Its restarts of the ESP32 are refused while the console is on.
- Pairing runs at boot while no controller is known. BlueRetro's BLE configuration page has no authentication, so it's closed at boot, and `oxctl bt config on` opens it. Its firmware update is turned off: OpenOX's firmware is updated through its own paths.
- Without standby power, controllers reconnect each time the console turns on, which takes 1 to 3 seconds.

## SMBus

The console's SMBus (LPC header pins 13 and 14) is wired to the ESP32. It's slow and has no hard timing.

- **Master** (I2C0, 100kHz): reads the SMC (`0x10`) for the tray, fan and boot reason, the temperature sensor (`0x4C`) and the EEPROM (`0x54`), and sends the SMC's reset, power cycle and power off. The MCPX is the other master: transfers are short, start only once the bus is idle, and lost arbitration is retried. The master only exists for one transfer and is torn down after, so a failed transfer can't leave the console's bus held, which would make its boot fail.
- **Slave** (I2C1 on the same pins, address `0x60`): the default state, back after every master transfer. No clock stretching, so it never holds the bus. For now it only acknowledges its address.

Turning the console on isn't possible over SMBus. That's the front-panel button, below.

## Console power

- **State** comes from the FPGA: no LCLK is off, LCLK with `LRESET#` low is reset, both is running. The FPGA raises an interrupt when LCLK starts or stops and on resets, and a task reads the state on each and once a second. Off is only visible with the device on standby power.
- **Buttons:** the power and eject switch lines, on open-drain GPIOs, pressed for 200ms. Power boots the user's ROM, eject boots System.
- **Wake:** when a Bluetooth controller connects and the console is off, the power button is pressed, at most once every 15 seconds.
- **Off:** once the console has been off for 5 seconds after running, the Bluetooth controllers are disconnected. Their pairing stays, and the Home button reconnects one, which wakes the console.

## PC protocol

`host_link` runs on UART0 at 921600 8N1. It's binary: [oxctl](../user/oxctl.md) is the human interface, and `tools/oxlink.py` its protocol side.

- **Framing:** COBS, frames delimited by `0x00`. Before encoding, a frame is `type (1) | seq (1) | len (2, LE) | payload | CRC-32 (4, LE)`. Anything that fails the CRC is dropped, so the boot ROM's text before the app starts is ignored.
- **Flow:** the PC sends requests, and every request gets exactly one response with the same `seq`: `OK` with a payload, or `ERR` with a code. The ESP32 also sends `EVENT` frames on its own.
- **Logs:** once the app runs, ESP-IDF's logging goes out as `LOG` frames, and nothing else is written to UART0.

| Commands | Group |
|---|---|
| `0x00xx` | `sys`: ping, version, status, reboot |
| `0x01xx` | `log`: level per tag |
| `0x02xx` | `fpga`: hold, release, the flash with the FPGA held |
| `0x03xx` | `link`: register reads and writes, probe, clock, interrupt counts |
| `0x04xx` | `smbus` and `xbox`: SMBus reads and writes, SMC power, the console's state and buttons |
| `0x05xx` | `prog`: one [programmer protocol](programmer-protocol.md) packet in, its reply out. ROMs, uploads and updates all go this way, so the PC gets everything the console does |
| `0x06xx` | `nor`: the flash through the running FPGA |
| `0x07xx` | `usb` |
| `0x08xx` | `bt` |
