# OpenOX LPC device documentation

## Using it

- [Wiring](user/wiring.md): the UPduino and ESP32 devkit on the console's LPC header
- [ROMs](user/roms.md): the ROM library, what power and eject boot, uploading
- [oxctl](user/oxctl.md): every command of the PC tool

## Developing it

- [Architecture](dev/architecture.md): what each chip does and why
- [Building](dev/building.md): the gateware, the firmware and `libs/oxprog`
- [FPGA](dev/fpga.md): the designs and the gateware's blocks
- [ESP32](dev/esp32.md): the firmware's components
- [Link](dev/link.md): the signals, SPI link and registers between the ESP32 and the FPGA
- [Boot flow](dev/boot-flow.md): ROM roles, one-shot boots, failed boots and trial updates
- [Programmer protocol](dev/programmer-protocol.md): how the console and the PC talk to the ESP32
- [Measurements](dev/measurements.md): cold-boot timing and other bus traffic seen on a real console
- [Status](dev/status.md): what's planned and not built yet

[Credits](credits.md)
