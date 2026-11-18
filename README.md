# OpenOX LPC peripheral (WIP)

An LPC peripheral for the original Xbox, built from an iCE40UP5K FPGA and an ESP32-PICO-MINI-02. It serves the BIOS from a library of ROMs, gives the console a COM port for the kernel debugger, and adds a USB controller with Bluetooth gamepads.

The FPGA does everything with hard timing and boots on its own, fast enough to serve the BIOS on a cold boot. The ESP32 does everything else: the ROM library, updates, the PC connection, USB devices and Bluetooth.

It's developed on a UPduino v3.1 and an ESP32-PICO-DevKitM-2. There's no custom board yet.

## What it does

- Serves 256KB, 512KB and 1MB BIOS images from its flash, on the 1.6 through the LFRAME# abort
- A ROM library with a Loader that boots the user's ROM on power and OpenOX's System on eject, one-shot boots and trial updates (see [Boot flow](docs/dev/boot-flow.md))
- A 16550 COM port with SMSC Super I/O detection, for the kernel debugger
- A programmer protocol for console software, on four LPC I/O ports (see [Programmer protocol](docs/dev/programmer-protocol.md))
- A USB hub with an Xbox gamepad on the console's controller port, driven by Bluetooth controllers through BlueRetro
- The console's power and eject buttons and its SMBus, from the PC
- Updates to its own FPGA bitstream and ESP32 firmware while the console runs

## Repository layout

| Directory | Contents |
|---|---|
| `fpga/` | The gateware: `rtl/`, the UPduino designs in `boards/`, testbenches in `sim/` |
| `esp32/` | The ESP32 firmware, an ESP-IDF project |
| `libs/oxprog/` | The programmer protocol's definitions and client, shared with OpenOX |
| `tools/` | `oxctl.py`, the PC tool |
| `thirdparty/` | ESP-IDF |
| `docs/` | Documentation |

## Documentation

See [docs/README.md](docs/README.md).

## License

Apache-2.0, see [LICENSE](LICENSE). Submodules keep their own licenses.
