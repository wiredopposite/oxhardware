# Building

## FPGA

Needs [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build) (Yosys, nextpnr-ice40, icepack, iceprog and Icarus Verilog), with its `bin` and `lib` folders on `PATH`. On Windows, running its `environment.ps1` does that. It also needs GNU Make and Python 3, which OSS CAD Suite doesn't include.

From `fpga/`:

| Command | What it does |
|---|---|
| `make DESIGN=lpc_com` | Builds a design for the UPduino. `hello` by default |
| `make DESIGN=lpc_com prog` | Builds it and writes it to the UPduino's flash with `iceprog` |
| `make rom ROM=<file>` | Writes a BIOS image to the flash at `0x40000`, slot 0 |
| `make DESIGN=lpc_com sim` | Runs the design's testbench, `sim/tb/tb_<design>.v` |
| `make sim-usb` | Runs the USB device testbench |
| `make clean` | |

The output is `build/upduino31/<design>/top_<design>.bin`. `lpc_com` is the one the ESP32 expects. Once the ESP32 is running, update it with `oxctl update fpga` instead of `iceprog`.

`iceprog` uses the UPduino's own USB programmer, which shares the flash pins with the ESP32. Don't run it while the ESP32 is using the flash.

## ESP32

Needs ESP-IDF, which is the `thirdparty/esp-idf` submodule, and BlueRetro, the `esp32/components/blueretro` submodule:

```
git submodule update --init esp32/components/blueretro
git submodule update --init --recursive thirdparty/esp-idf
thirdparty/esp-idf/install.sh esp32
```

On Windows, use `install.ps1` (or `install.bat`) instead of `install.sh`. Then, in a terminal with ESP-IDF's environment (`export.sh` or `export.ps1`), from `esp32/`:

```
idf.py build
idf.py -p COMx flash monitor
```

The first flash goes over the devkit's USB port. After that, `oxctl update esp32 esp32/build/openox.bin` updates it without the ESP-IDF tools, and keeps the old firmware if the new one doesn't start.

## libs/oxprog

A plain CMake project with no dependencies besides C++20:

```
add_subdirectory(path/to/libs/oxprog)
target_link_libraries(myapp PRIVATE oxprogclient)
```

| Target | Contents | For |
|---|---|---|
| `oxprog` | `oxprog/oxprog.hpp`: ports, packets, enums, bank helpers, `Crc32`, `StatusName` | Both ends |
| `oxprogclient` | `oxprog/client.hpp`: `Transport` (a byte stream the host provides) and `Client` (one call per command, uploads and downloads in chunks) | Hosts |
| `oxprogc` | `oxprog/oxprog.h`: the client for C, with the transport as callbacks. A shared library with `BUILD_SHARED_LIBS` | Hosts in C, and bindings from other languages |

Built on its own, it also builds `oxprog_c_test`, which runs the C API against a fake device. In an ESP-IDF build the same folder is a component that provides `oxprog` only.

## oxctl

Needs Python 3 and `pyserial` (`pip install pyserial`). See [oxctl](../user/oxctl.md).

## VS Code

`openoxfw.code-workspace` opens `fpga/` and `esp32/` as separate folders: the ESP-IDF extension and clangd from esp-clang for the firmware, and OSS CAD Suite on the terminal's `PATH` for the gateware. For that, set an `OSS_CAD_SUITE` environment variable to where OSS CAD Suite is installed, then restart VS Code.

In the `fpga` folder, Run Task has the `make` targets above: Build (Ctrl+Shift+B), Build and program, Write BIOS image, Simulate, Simulate USB and Clean.
