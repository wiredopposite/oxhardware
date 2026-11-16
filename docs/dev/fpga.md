# FPGA

The gateware for the iCE40UP5K, in `fpga/`. Building it is in [Building](building.md#fpga).

| Directory | Contents |
|---|---|
| `rtl/lpc/` | The LPC target, COM port, Super I/O, boot control port, mailbox, `LRESET#` filter, LCLK detector |
| `rtl/mem/` | Reading the flash (`spi_stream`) and lending it to the ESP32 (`flash_share`) |
| `rtl/boot/` | Banks, armed boots and boot flags (`bank_ctrl`) |
| `rtl/link/` | The ESP32 link and its identity registers |
| `rtl/usb/` | The USB full-speed device |
| `rtl/uart/`, `rtl/common/` | UART, FIFOs, SPRAM wrapper, report helpers |
| `rtl/probe/` | Bus sniffing for the probe designs |
| `boards/upduino31/` | The UPduino's pin map, clocks and top-level designs |
| `sim/` | Icarus Verilog testbenches and a SPI flash model |
| `tools/` | Viewers for the probe designs' reports, `bin2hex` |

## Designs

Each design is a `top_<name>.v` in `boards/upduino31/`.

| Design | What it is |
|---|---|
| `lpc_com` | The device: BIOS serving, the COM port and Super I/O, the boot control port, the mailbox, flash lending, USB and the ESP32 link |
| `lpc_serve` | BIOS serving on its own, with a logic analyser capture of the bus. Used to bring up serving on the 1.6 |
| `lpc_probe` | Passive: times a cold boot and reports it over the UART ([Measurements](measurements.md#cold-boot-timing)) |
| `lpc_iolog` | Passive: logs every LPC cycle except memory reads, and power and `LRESET#` events, with timestamps ([Measurements](measurements.md#a-xenium-on-the-same-bus)) |
| `hello` | The toolchain check: the PLL, the LED, and a UART banner and echo |

The rest of this page is about `lpc_com`.

## LPC target

`lpc_target` runs on LCLK and answers two kinds of cycle:

| Cycle | Clocks |
|---|---|
| Memory read | START, CYCTYPE `010x`, A31 to A0 (8), TAR (2), SYNC, D3:0, D7:4, TAR (2) |
| I/O read | START, CYCTYPE `000x`, A15 to A0 (4), TAR (2), SYNC, data (2), TAR (2) |
| I/O write | START, CYCTYPE `001x`, A15 to A0 (4), data (2), TAR (2), SYNC, TAR (2) |

- **START** comes from `LAD` alone: a `0000` nibble while it's between cycles. Cycles it doesn't claim are followed to their end, so a `0000` SYNC or data nibble is never taken for a START. `LFRAME#` isn't needed, which matters on 1.3 to 1.5 (no `LFRAME#` on the header) and on 1.6 (where the FPGA drives it).
- **Memory reads** in `FFxxxxxx` are claimed while serving is on, and answered with long-wait SYNC until the byte arrives.
- **I/O cycles** are offered to the ports on the I/O bus (COM, Super I/O, boot control, mailbox). A port that claims it gets a ready SYNC, with no wait states. Nothing else is answered.
- **TAR:** it drives `1111` on the first TAR clock after its SYNC or data, then lets go.
- **Reset:** `LRESET#` has to stay low for 16 LCLK to count (`lreset_filter`). In reset the target goes idle and lets go of `LAD`, and the COM port, Super I/O and mailbox go back to their defaults.

### Flash address

`flash address = ROM base + bank slot × 256KB + (A19:A0 & size mask)`

A 256KB or 512KB image mirrors through the 1MB window, and the window through all of `FF000000` to `FFFFFFFF`. The ROM base on the UPduino's 4MB flash is `0x40000`.

### Reading the flash

`spi_stream` reads the flash with single-bit `READ` (`0x03`) at LCLK/2, about 16.7MHz. After a read it keeps the flash selected and streams the following bytes into a two-byte buffer, so sequential reads, which is nearly all of a boot, cost a few SYNC waits each. A read elsewhere restarts it, which takes about 2.5µs. It wakes the flash from deep power-down first, since the iCE40 can leave it there after configuration.

### LFRAME# abort (1.6)

On a 1.6 the onboard flash, behind the Xyclops, answers every memory read too. A claimed read is hidden from it by holding `LFRAME#` low: a target takes START from the last clock with `LFRAME#` low, so while it stays low the other device keeps waiting for START and never decodes the cycle.

- **Per cycle:** once a memory read is claimed, `LFRAME#` is held low from the address phase through the second TAR clock. Holding it during CYCTYPE would look like a new START. The last clock with it low has `LAD = 1111`, which isn't a START either.
- **Boot hold:** from `LRESET#` until the read of `FF000074` has been served, `LFRAME#` is held low the whole time, then the per-cycle aborts take over, so the first fetches don't depend on how fast the switch turns on and off. A design loaded while the console runs (an update) takes no hold, since a running console never fetches `..74` again.
- **Drive:** an iCE40 pin can't pull `LFRAME#` low against the MCPX, so it switches an N-MOSFET (see [Wiring](../user/wiring.md#lframe-switch-16)), and reads `LFRAME#` back on a separate input. The switch leaves `LFRAME#` low for about 2 LCLK after it's released, which the MCPX tolerates.

## COM port

`com16550` is the guest side of a 16550 at `0x3F8`, or wherever the guest places it through the Super I/O.

- RBR/THR, IER, IIR/FCR, LCR, MCR, LSR, MSR and SCR, and DLL/DLM behind DLAB.
- The guest's divisor is stored and read back, but never used. The UART runs at a fixed 115200 8N1.
- LSR reports THRE and TEMT only while the transmit FIFO has room, so a polled sender waits instead of losing bytes.
- No interrupts: IIR always reads none pending.
- On reset the registers go back to their defaults and the receive FIFO is emptied. The transmit FIFO keeps draining, so the last output before a reboot isn't lost.

### Super I/O

`superio_cfg` is an SMSC-style configuration space, enough for a kernel to find the serial port and place it. `0x55` to `0x2E` enters configuration mode and `0xAA` leaves it. Register `0x07` selects the logical device. Devices 4 and 5 are serial ports: active when bit 0 of `0x30` is set, at the base in `0x60` and `0x61`. Registers `0x26` and `0x27` report the Super I/O's own configuration port, `0x2E`. The data port only answers in configuration mode.

The devkit kernel probes for this at boot and turns on serial debug output and its debugger stub only if it answers (see [Measurements](measurements.md#a-xenium-on-the-same-bus)).

## Banks and the boot control port

`bank_ctrl` keeps the active, default and pending banks, arms one-shot boots and keeps the boot flags. At each `LRESET#` assertion an armed bank becomes the active one for one boot, otherwise the default does. `bootctl_port` is the console's side of it: eight I/O ports at `0x00E0`. Both are described in [Boot flow](boot-flow.md).

The banks start from build parameters: serving on, `LFRAME#` abort on, the default bank slot 0. The ESP32 sets the rest once it's up.

## Mailbox

`mailbox` is the console port of the [programmer protocol](programmer-protocol.md#console-port): four I/O ports at `0x0B00`, with a 512-byte FIFO each way to the ESP32 over the link.

## Flash lending

`flash_share` hands the flash to the ESP32. When the ESP32 asks, and the console has gone 1ms without a BIOS read (or is in reset), it stops the flash reader, floats its flash pins and grants it. Until the ESP32 gives it back, BIOS reads are answered with 0xFF at once and counted. The registers are in [Link](link.md#flash).

## USB device

`USB_DP`, `USB_DN` and `USB_PU` (1.5kΩ to D+; floating is detached), with 27Ω in series on D+ and D-, to the console's controller port.

```
 D+/D- ─► sample (48MHz, 4×) ─► NRZI decode ─► unstuff, bytes, CRC5/16 ─► events ─┐
                                                                                  │
 usb_sie (24MHz): token ─► address map ─► endpoint map ─► slot state ─► handshake │
                                                                                  │
 D+/D- ◄─ NRZI encode ◄─ stuff ◄─ SYNC, PID, data, CRC16, EOP (48MHz) ◄── bytes ──┘
```

- **PHY** (`usb_phy_rx`, `usb_phy_tx`, 48MHz): the receiver resynchronises on every transition and samples two clocks later. NRZI decode, unstuffing, byte assembly and both CRCs are serial. Counters are one-hot and the transmitter decides each bit boundary one clock early, to keep 48MHz paths at one or two LUT levels. Events and transmit bytes cross to the SIE with toggles.
- **SIE** (`usb_sie`, 24MHz): table lookups instead of compares. The address map and slot state share one block RAM owned by the engine. The endpoint map and IN lengths are another, written by the link. The address map is read as soon as the token's address byte is in, so the handshake is ready when the EOP arrives. Measured turnaround is 4.5 bit times (the limit is 7.5).
- **Buffers:** IN 16 × 64 bytes (the link writes, the SIE reads). OUT 16 × 64 bytes, SETUP 8 × 8 bytes and lengths (the SIE writes, the link reads). One writer and one reader per RAM, so neither side waits.
- **Queue:** slot commands, arms and address-map writes from the link go into a 256-entry queue that runs between transactions, so state never changes under an open transaction.
- Up to 8 devices at different addresses, and 32 endpoint slots (16 IN, 16 OUT).
- SOF frame number. Suspend after about 5ms of idle J.

The registers and the behaviour the ESP32's driver relies on are in [Link](link.md#usb).

## LED

Blue while serving reads, green when idle, red for COM output, blinking red when there's no 12MHz clock.

## Resources and timing

`lpc_com` takes 4479 of the UP5K's 5280 logic cells (84%). Its clocks reach:

| Clock | Needs | Reaches |
|---|---|---|
| LCLK | 33.3MHz | 36.0MHz |
| USB line side | 48MHz | 55.0MHz |
| System | 24MHz | 31.6MHz |
| Link | 20MHz | 61MHz |

On the UP5K, routing rather than logic dominates fast paths: typical failing paths at 48MHz were 6 to 8ns of logic and 14 to 16ns of routing. Wide counter compares, 16:1 register muxes feeding an output and enables with many conditions each failed 48MHz until pipelined. So in fast logic:

- Flag a counter's terminal count a cycle early.
- Register mux outputs.
- Decode events into registered one-hot flags.
- Count up from zero, and avoid wide magnitude compares.

## Testbenches

| Testbench | Covers |
|---|---|
| `tb_lpc_com.v` | The `lpc_com` design: BIOS reads, the Super I/O sequence a debug kernel uses to find its serial port, and COM traffic both ways through the UART pins |
| `tb_lpc_serve.v`, `tb_lpc_probe.v`, `tb_lpc_iolog.v` | The other designs |
| `tb_usb.v` | The USB device: a host model with its bit clock 0.25% off drives control transfers, a deferred SET_ADDRESS, a second device at address 0, interrupt and bulk endpoints, NAK, STALL, toggles, retries, bad CRCs, SOF and bus reset |
