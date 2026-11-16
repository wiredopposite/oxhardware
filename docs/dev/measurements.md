# Measurements

What the probe designs (`lpc_probe`, `lpc_iolog`, `lpc_serve`) saw on a real 1.6 console. The designs are in [FPGA](fpga.md#designs).

## Cold-boot timing

`lpc_probe` on a USB-powered UPduino, with passive inputs on the 3.3V rail (sense), `LRESET#`, `LFRAME#` and `LCLK` through series resistors. It timestamps rail up, `LRESET#` and the first START, and reports over its UART (`fpga/tools/lpc_probe_view.py`).

A 1.6 with nothing else on the bus, 6 cold boots:

| | LCLK starts | `LRESET#` released | First LPC cycle |
|---|---|---|---|
| Typical (5 of 6) | 97.05 to 97.23ms | 98.19 to 98.37ms | 0 to 1µs later |
| Earliest (1 of 6) | 77.1ms | 78.26ms | 1µs later |

- The sequence is the same every boot: `MEM RD FF000070..77`, then sequential single-byte reads from `FF000000`. 268,621 memory reads, 1 I/O read and 2 I/O writes in the first 4.2 seconds.
- Reads complete every 1.1µs or so. The stock flash already inserts SYNC waits and the MCPX accepts them.
- LCLK runs for about 1.1ms before `LRESET#` is released.
- The 78ms boot, probably a faster rail rise on a quick power cycle, sets the budget: the FPGA's configuration and start-up have to finish within about 70ms.

Not measured yet: 1.0 to 1.5 boards, and the FPGA's own time from power to serving.

## Serving

`lpc_serve` on a 1.6, reading single-bit SPI flash at LCLK/2: a full boot of a 256KB image served 271,019 reads with no SYNC timeouts and no data errors. The longest SYNC wait was 87 LCLK (about 2.6µs), which the MCPX accepted.

## Boot outcomes

Used to judge armed boots (see [Boot flow](boot-flow.md#failed-boots)). A 1.6 with a 256KB image:

| | Good boot | Failed boot (a bad image, or nothing served) |
|---|---|---|
| Memory reads after `LRESET#` goes high | About 267K, all within 0.5s | Stop at 1.4K to 2.5K |
| `LRESET#` | Stays high | The SMC retries: bursts of 4 resets about 0.37s apart, three bursts, then FRAG with no more resets |
| SMC over SMBus during the FRAG | | Still answers reads |

## A Xenium on the same bus

`lpc_iolog` on a 1.6, with a Xenium-compatible device running XeniumOS on an LPC splitter: power on, the XeniumOS menu for about 2 minutes, then a BIOS booted from bank 3. Memory reads aren't logged, so which device served which read isn't visible.

| Port | Seen | Meaning (inferred) |
|---|---|---|
| `0xEE` write | 21 writes, values `00` to `07` only. At boot it steps `00` to `07` within 26ms (an LED self-test), then `02`, `03`, `07`, `01` and so on. `00` just before the final reset | A 3-bit LED colour |
| `0xEF` write, bits 3:0 | Values `1`, `2`, `3` and `A`. `2` at 620ms, then `A` and `1` alternate while the menu runs. `3` is the bank booted | Bank select. XeniumOS switches banks at runtime, presumably to read its BIOS list and settings, so a bank write has to apply at once, not only at reset |
| `0xEF` write, bits 6:4 | Idle at `111` (`0x70`). Bursts of 131, 520 and 286 writes in which bit 6 toggles every write pair and bit 4 carries data. No reads of `0xEF` during bursts | Write-only bit-banged serial: bit 6 clock, bit 4 data, bit 5 select (low during bursts). Consistent with an SPI LCD. About 3 I/O writes, 46µs, per bit |
| `0xEF` write, bit 7 | Set once: `73` then `F3`, and `LRESET#` asserts 1.06ms later | Boots the selected bank. The reset follows |
| `0xEF` read | Once, at 2.05s, returning `BA` | Device detect or status |
| `0x3F8` to `0x3FD` | XeniumOS (RTTarget-32) programs COM1 (divisor 1, 115200 8N1, MCR `03`) and prints its banner. Its LSR reads get no response | Real software uses COM1 at boot |
| `0x2E`, `0x2F` | After the booted devkit BIOS starts: write `55`, `26`, read `2F`, no response, and no COM traffic follows | The kernel's Super I/O probe: SMSC enter-config key `55`, then index `0x26`. With nothing answering, it skips serial debug output and its debugger stub |
| `LRESET#` | After `F3`: asserted 1.06ms, released 1.1ms later, asserted again 21ms later, released 21ms after that | A bank-switch reboot has two reset pulses. The bank written before them has to survive both |
