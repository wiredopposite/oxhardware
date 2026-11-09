# Credits and provenance

## Provenance

The gateware, tools and documentation in this repository are original work. **No source code from the third-party projects listed below is included.** They were studied as references for how the original Xbox's LPC bus, boot process and modchip registers behave.

The primary sources for that behaviour are:

- Intel *Low Pin Count (LPC) Interface Specification*, rev. 1.1.
- Lattice iCE40 UltraPlus and Winbond W25Q-series datasheets.
- The Xbox Dev Wiki (xboxdevwiki.net).
- Measurements on a real console with this project's own probe designs (`lpc_probe`, `lpc_iolog`, `lpc_serve`), recorded in [00-system.md](00-system.md) §4.4 and [02-fpga.md](02-fpga.md) §6.2.

Facts such as LPC cycle formats, the 1.6 LFRAME# abort, Xenium-compatible I/O ports and the Super I/O enter-config key are interoperability information. They are implemented here from those sources.

## The author's own projects

Code from these may be reused; it is the author's own work.

- **ox**: RP2350-based Xbox LPC chip.
- **xprobe**: LPC/Super I/O probe.
- **pico_usb**: USB device stack with pluggable controller drivers and hub emulation.

## Third-party projects consulted

| Project | Licence (as published) | Consulted for |
|---|---|---|
| OpenXenium (Ryzee119) | Firmware GPLv3, hardware CERN-OHL-1.2 | 1.6 LFRAME# abort via MOSFET, boot hold until read `..74`, Xenium register behaviour |
| EOS | See its repository (parts GPLv3) | LFRAME# abort window and pad drive notes |
| valentyusb | BSD-3-Clause | USB full-speed PHY/SIE architecture on iCE40 |
| FPGA-USB-Device | LGPL-2.1 | USB packet layer structure (study only) |
| Gowin USB 1.1 SoftPHY reference design | Vendor licence | USB soft PHY approach (study only) |
| lpc-uart | See its repository | Minimal LPC-to-UART device |
| BlueRetro (darthcloud) | Software Apache-2.0, hardware CERN-OHL-P-2.0 | ESP32 Bluetooth controller stack (planned reuse) |

## Reuse policy

- **Code from permissively licensed projects** (Apache-2.0, BSD, MIT, e.g. BlueRetro, valentyusb) may be reused. It keeps its original licence headers and notices, and is listed here when it is added.
- **Code from GPL/LGPL projects, or from projects with no licence file,** is not copied into this repository.
