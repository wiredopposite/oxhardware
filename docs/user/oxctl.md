# oxctl

`tools/oxctl.py` talks to the ESP32 over its USB serial port (UART0, 921600 8N1), using the [PC protocol](../dev/esp32.md#pc-protocol). It's the only way into the device from a PC: ROMs, the console's power and SMC, chip updates, the controller, Bluetooth and the kernel debugger.

It needs Python 3 and `pyserial`.

```
python tools/oxctl.py -p COMx <group> <command> [args]
```

| Option | |
|---|---|
| `-p`, `--port` | The ESP32's serial port. Defaults to the `OXCTL_PORT` environment variable. |
| `-b`, `--baud` | 921600 by default. |
| `-v`, `--verbose` | Also prints the ESP32's logs and events while the command runs. |

Numbers (addresses, lengths, IDs, registers) take decimal or `0x` hex.

## ROMs

The ROM library is in the flash behind the FPGA, see [ROMs](roms.md). Entries have an ID, a role (`user`, `loader`, `system`, `diag`), a slot, a size and a name.

| Command | |
|---|---|
| `rom list` | Every entry. The last column shows its flags: `current` (the Loader, System or Diag in use), `trial`, `failed-boot` and `user` (the power-button ROM). |
| `rom upload FILE [options]` | Uploads a ROM and adds it to the library. See below. |
| `rom download ID OUT` | Saves an entry to a file. `rom download active OUT` saves the ROM the console is being served. |
| `rom delete ID` | Deletes an entry. |
| `rom rename ID NAME` | Up to 32 characters. |
| `rom user ID` | Makes entry `ID` the power-button ROM. `rom user none` clears it. |
| `rom boot ID [--reset]` | Boots `ID` once, from the console's next reset. `--reset` resets the console now. |
| `rom compact SIZE` | Moves entries to free a run of `SIZE` (`256K`, `512K` or `1M`). |

`rom upload` options:

| Option | |
|---|---|
| `--name NAME` | The entry's name. Defaults to the file name without its extension. |
| `--type user\|loader\|system\|diag` | Its role, `user` by default. |
| `--user` | Make it the power-button ROM. |
| `--boot` | Boot it once, from the console's next reset. |
| `--reset` | Reset the console once it's uploaded. With `--boot`, that boots it straight away. |
| `--trial` | A Loader or System: it boots once on trial and only becomes current once it confirms ([Boot flow](../dev/boot-flow.md#trial-updates)). |
| `--compact` | If there's room in total but no free run of the right size, move entries to make one. Without it, such an upload fails with `Fragmented`. |

Turn the console off before uploading (`xbox off`). The flash is shared with the console while it runs, and an upload with the console on fails its CRC check.

Examples:

```
python tools/oxctl.py -p COMx xbox off
python tools/oxctl.py -p COMx rom upload devkit.bin --name devkit --user
python tools/oxctl.py -p COMx xbox power
```

```
python tools/oxctl.py -p COMx rom upload test.bin --name test --boot --reset
```

The second boots `test` once and leaves the power-button ROM alone. The console must be off for the upload, so `--reset` there only matters if it was turned on in between. Usually you press power afterwards.

## The console

| Command | |
|---|---|
| `xbox state` | `off`, `reset` or `running`, from the FPGA. |
| `xbox power` | Presses the power button. |
| `xbox eject` | Presses the eject button. |
| `xbox status` | Tray state, AV pack and temperatures, read from the SMC. |
| `xbox reset` | Resets the console through the SMC. |
| `xbox cycle` | Power-cycles it through the SMC. |
| `xbox off` | Turns it off through the SMC. |
| `smbus read ADDR REG [--word]` | Reads the console's SMBus, e.g. `smbus read 0x10 0x03`. |
| `smbus write ADDR REG VALUE [--word]` | |

`xbox reset` works on a console that's hung with interrupts off.

## Kernel debugger

The console's COM1 is the FPGA's COM UART, at a fixed 115200 whatever the guest programs, on its own USB serial port. These commands take that port with `--com`, which defaults to the `OXKD_COM` environment variable. Only `kd debug` needs `-p`.

| Command | |
|---|---|
| `kd debug BIOS --com COMy` | The whole loop. Turns the console off if it's on, uploads `BIOS` (a KD build) as the power-button ROM named `kd`, replacing the last `kd`, and arms it for the next boot. Then it turns the console on, waits for the kernel's boot break and bridges COM1 to TCP for lldb. If the kernel doesn't answer, it arms the ROM again and resets the console once more. |
| `kd bridge --com COMy` | Only the bridge, for a console that's already running a KD build. |
| `kd probe --com COMy` | Asks the kernel why it stopped and prints the answer. |
| `kd kill --com COMy` | Resets a console that's stopped in the debugger. |

| Option | |
|---|---|
| `--com-baud` | 115200, the FPGA's UART speed. |
| `--tcp` | The TCP port for lldb, 2345 by default: `gdb-remote 2345`. |
| `--name` | The ROM name `kd debug` uses and replaces, `kd` by default. Give ROMs you want to keep a different name. |
| `--wait` | Seconds to wait for the kernel to answer, 20 by default. |
| `--traffic` | Prints the GDB remote protocol traffic. |

The bridge takes one debugger at a time and drops what the kernel sends while none is connected. It keeps running until Ctrl+C. The OpenOX side of the loop, VS Code included, is in OpenOX's `docs/dev/debugging.md`.

## Chip updates

| Command | |
|---|---|
| `update fpga FILE` | Writes a new bitstream and reloads the FPGA, which takes a few seconds. The console keeps running. |
| `update esp32 FILE` | Writes new ESP32 firmware. The ESP32 restarts once it's written. |
| `tables upload FILE [--source CRC]` | Uploads the retail tables the System BIOS is built from. `--source` is the CRC of the retail BIOS they came from. |
| `tables download OUT` | |

## Controller and Bluetooth

| Command | |
|---|---|
| `usb status` | The controller's USB state. |
| `usb stop`, `usb start` | Detaches the controller until it's started again or the ESP32 reboots. |
| `usb unplug`, `usb plug` | Unplugs the gamepad from hub port 1, and plugs it back in. |
| `usb pad BUTTONS... [--lx N] [--ly N] [--rx N] [--ry N]` | Holds buttons and sets the sticks (-32768 to 32767), e.g. `usb pad a start --lx 20000`. Prints the last rumble. |
| `bt status` | Pairing, connected controllers and the config page. |
| `bt pair on\|off` | Pairing runs by itself at boot while no controller is known. |
| `bt config on\|off` | Opens BlueRetro's BLE configuration page. It has no authentication, so it's closed at boot. |
| `bt forget` | Disconnects every controller and clears all pairing keys. |

## ESP32

| Command | |
|---|---|
| `sys ping`, `sys version`, `sys status` | |
| `sys reboot` | Restarts the ESP32. |
| `log level LEVEL [TAG]` | `none`, `error`, `warn`, `info`, `debug` or `verbose`, for every tag or one. |
| `monitor [--reset]` | Prints logs and events until Ctrl+C. `--reset` restarts the ESP32 first, to see its boot log. |

## Bring-up and diagnostics

These are for working on the device itself.

| Command | |
|---|---|
| `fpga status` | |
| `fpga id` | The FPGA flash's JEDEC ID. |
| `fpga probe` | Checks the configuration bus wiring. |
| `fpga write FILE [--addr A]` | Programs the FPGA's flash directly. |
| `fpga read OUT --addr A --len N` | |
| `fpga hold`, `fpga release` | Holds the FPGA in reset, or lets a held one boot. |
| `link info` | The FPGA's identity over the SPI link. |
| `link probe` | Checks the link wiring. |
| `link test [--count N] [--mhz F]` | Stress-tests the scratch register. `--mhz` sets the link clock first, kept until the ESP32 reboots. |
| `link read ADDR [--len N] [--space S]`, `link write ADDR HEXBYTES [--space S]` | FPGA registers, see [Link](../dev/link.md#registers). |
| `link irq` | FPGA interrupts taken, by bit. |
| `nor info` | The ROM flash, through the running FPGA. |
| `nor read OUT --addr A --len N` | |
| `nor write FILE --addr A` | Erases, programs and verifies. |
| `nor erase --addr A --len N`, `nor crc --addr A --len N` | |
| `prog info` | The [programmer protocol](../dev/programmer-protocol.md). |
| `prog smbus-read ADDR REG`, `prog smbus-write ADDR REG VALUE` | SMBus through the programmer. |
| `prog confirm` | Confirms the trial boot that's running now. |

`fpga id`, `read`, `write` and `hold` keep the FPGA in reset (CRESET low) while they run. The console loses its BIOS meanwhile, and `iceprog` mustn't be running. `nor write` and `nor erase` write the ROM library's flash directly, outside the catalogue.
