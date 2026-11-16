# Link

Everything between the ESP32 and the FPGA: the signals, the config bus, the SPI link and the FPGA's registers. The pins on the bench are in [Wiring](../user/wiring.md).

## Signals

### Config bus

The FPGA's configuration pins, shared by the FPGA, the flash and the ESP32.

| Net | FPGA pin | Flash pin | ESP32 | Normally | While the ESP32 has the bus |
|---|---|---|---|---|---|
| `CFG_SCK` | `SPI_SCK` | CLK | SCK | FPGA drives | ESP32 drives |
| `CFG_SS_N` | `SPI_SS_B` | CS# | GPIO | FPGA drives | ESP32 drives |
| `CFG_SO` (FPGA to flash) | `SPI_SO` | DI | MOSI | FPGA drives | ESP32 drives |
| `CFG_SI` (flash to FPGA) | `SPI_SI` | DO | MISO | Flash drives | Flash drives |

The ESP32 leaves its config-bus pins floating unless it's holding `CRESET_B` low or the FPGA has lent it the flash.

### Link and control

| Signal | Direction (from the ESP32) | Notes |
|---|---|---|
| `LINK_SCK`, `LINK_MOSI`, `LINK_MISO`, `LINK_CS_N` | Out, out, in, out | The FPGA floats MISO while `LINK_CS_N` is high. The bus is the FPGA's alone |
| `FPGA_IRQ_N` | In | Active low while `IRQ_STATUS & IRQ_ENABLE` isn't 0 |
| `FPGA_CRESET_N` | Out, open-drain | Low holds the FPGA unconfigured, with all its pins floating on weak pull-ups |
| `FPGA_CDONE` | In | High once configured |

## Who has the flash

- **Normally:** at power-up the FPGA loads its bitstream from flash offset 0 and starts serving. The ESP32 boots alongside and connects over the link.
- **The FPGA lends it:** the ESP32 asks over the link, and reads, erases and programs the flash while the FPGA answers BIOS reads with 0xFF. See [Flash](#flash).
- **The FPGA in reset:** the ESP32 holds `CRESET_N` low and drives the config bus as a plain SPI master (JEDEC ID `0x9F`, sector erase `0x20`, block erase `0xD8`, page program `0x02`, read `0x03`, status `0x05`). It then floats its pins and releases `CRESET_N`, and the FPGA reboots from the flash. The bitstream area is only written this way, or while lent with the FPGA reloaded afterwards.

## SPI link

SPI mode 0, MSB first, framed by `LINK_CS_N`, at 20MHz at most. The ESP32 routes it through the GPIO matrix, whose input delay plus the FPGA's output delay has to fit in half a clock. The FPGA shifts on SCK and hands whole bytes to its system clock.

```
 write: MOSI | CMD | A23:16 | A15:8 | A7:0 | data ...
        MISO | STS |  0x00  |  0x00 | 0x00 |
 read:  MOSI | CMD | A23:16 | A15:8 | A7:0 | dummy | 0x00 ...
        MISO | STS |  0x00  |  0x00 | 0x00 | 0x00  | data ...
```

**CMD:** bit 7 read, bits 6:4 space, bit 3 no address increment. Reads have one dummy byte.

**STS**, sent back on every transaction:

| Bit | Meaning |
|---|---|
| 7 | An enabled IRQ is pending |
| 6 | `LRESET#` (1 = the console is running) |
| 5 | Out of reset |
| 4 | A USB interrupt is pending |
| 2 | Mailbox data from the console is waiting |
| 0 | The flash is lent to the ESP32 |

Transactions can be any length. The firmware keeps them to 4KB.

| Space | Contents |
|---|---|
| 0 | [Registers](#registers) |
| 1 | [USB](#usb) buffers and tables |
| 3 | FIFO ports, without address increment: port 1 is the mailbox |

## Registers

Space 0. 8-bit registers, multi-byte ones little-endian and committed when the highest byte is written. Addresses not listed read 0 and ignore writes.

### Identity and interrupts

| Address | Name | Access | Description |
|---|---|---|---|
| `0x00`-`0x03` | `ID` | RO | `'O','X','F','G'` |
| `0x04`-`0x06` | `VERSION` | RO | Major, minor, patch |
| `0x07` | `BOARD_ID` | RO | 1 for the UPduino |
| `0x08`-`0x09` | `FEATURES` | RO | Bit 0 BIOS serving, 1 COM, 2 Super I/O, 3 mailbox, 4 USB, 6 `LFRAME#` abort, 9 flash lending |
| `0x0C`-`0x0F` | `SCRATCH` | RW | For testing the link |
| `0x10`-`0x11` | `IRQ_STATUS` | W1C | Bit 0 `LRESET#` asserted, 1 `LRESET#` released, 3 USB, 5 mailbox, 8 console power (LCLK started or stopped), 11 boot control (the console armed a bank or confirmed) |
| `0x12`-`0x13` | `IRQ_ENABLE` | RW | The same bits |

### LPC and banks

| Address | Name | Access | Description |
|---|---|---|---|
| `0x20` | `LPC_CTRL` | RW | Bit 0 serving on, 2 `LFRAME#` abort, 3 arm at reset (self-clearing), 4 I/O ports on |
| `0x21` | `LPC_STATUS` | RO | Bit 0 `LRESET#`, 1 LCLK running, 2 served a read since reset, 3 a bank is armed |
| `0x22` | `ACTIVE_BANK` | RW | Bits 5:0 slot (256KB units from the ROM base), bits 7:6 size (0 256KB, 1 512KB, 2 1MB). The slot is aligned to the size |
| `0x23` | `PENDING_BANK` | RW | The same format. Served for one boot after an arm |
| `0x24` | `LPC_RESET_COUNT` | RO | Wraps |
| `0x27` | `DEFAULT_BANK` | RW | Served after a reset with nothing armed: the Loader |
| `0x28`-`0x2A` | `LAST_MEM_ADDR` | RO | Flash address of the last read served |
| `0x2C`-`0x2F` | `MEM_READ_COUNT` | RO | Reads served since reset |
| `0x38`-`0x39` | `BOOTCTL_BASE` | RW | The boot control port's I/O base, `0x00E0` by default |
| `0x3A` | `BOOT_FLAGS` | RW | See [Boot flow](boot-flow.md#boot-control-port). Writing bit 2 sets `TRIAL_PENDING`, bit 3 sets `LAST_FAILED`, bit 6 clears `TRIAL_CONFIRMED`. Other bits are ignored |
| `0x3B` | `USER_BANK` | RW | The power-button ROM |
| `0x3C` | `SYSTEM_BANK` | RW | The confirmed System |
| `0x3D` | `DIAG_BANK` | RW | Diag |

### Flash

| Address | Name | Access | Description |
|---|---|---|---|
| `0xC0` | `FLASH_CTRL` | RW | Bit 0 `REQ` (the ESP32 wants the flash), bit 1 `GRANT` (read-only: the FPGA's flash pins are floating) |
| `0xC1` | `FLASH_MISS` | RW | BIOS reads answered 0xFF while the flash was away. Saturates at 255, a write clears it |

Borrowing the flash:

1. Write `REQ = 1`. Once the console has gone 1ms without a BIOS read, or while `LRESET#` is asserted, the FPGA stops its flash reader, floats its flash pins and sets `GRANT`.
2. Poll `GRANT` (a boot reads the flash for about half a second), then drive the config bus.
3. Wait until the flash isn't busy, float the config-bus pins and write `REQ = 0`. The FPGA drives its pins again at once and its reader starts over, waking the flash first. Read `FLASH_MISS`.

### Mailbox

| Address | Name | Access | Description |
|---|---|---|---|
| `0x100` | `MBX_CTRL` | RW | Bit 0 enable (on by default). Bit 1 `RESET_REQ`: the console wrote `0x01` to Id and both FIFOs were dropped. Write 1 to clear it, which drops them again. Bit 2 `READY`: the ESP32's programmer is up. Id reads `'R'` while it's clear or `RESET_REQ` is set |
| `0x101`-`0x102` | `MBX_BASE` | RW | The console ports' I/O base, `0x0B00` by default |
| `0x103`-`0x104` | `MBX_RX_LEVEL` | RO | Bytes from the console waiting, 0 to 512 |
| `0x105`-`0x106` | `MBX_TX_FREE` | RO | Room for bytes to the console, 0 to 512 |

The FIFOs are 512 bytes each way, through space 3 port 1. A write queues bytes for the console (dropped when full, so check `MBX_TX_FREE`). A read takes bytes from the console (`0x00` once empty). The link fetches read data ahead of what it sends, but only bytes actually clocked out are taken, so a read of any length loses nothing. The mailbox IRQ is raised when data arrives in an empty FIFO or `RESET_REQ` is set, and STS bit 2 is high while either holds. Clearing `RESET_REQ` also drops anything queued for the console before the reset.

## USB

One full-speed port carries up to 8 devices (a hub and the devices behind it, each at its own address). There are 32 endpoint slots: 0 to 15 are IN, 16 to 31 OUT. Slots *d* and 16 + *d* are device *d*'s EP0 IN and OUT. The others are assigned through the endpoint map.

A token is resolved by table lookups: the address map (USB address to device), the endpoint map (device, endpoint and direction to slot), then the slot's state. Slot state and the address map belong to the USB engine. Slot commands, arms and address-map writes are queued and applied while no transaction is open, in the order written, so a write's effect isn't visible on the bus at once (normally within one packet time).

| Address | Name | Access | Description |
|---|---|---|---|
| `0x200` | `USB_CTRL` | RW | Bit 0 pull-up (attached), bit 1 SIE on |
| `0x201` | `USB_STATUS` | RO | Bit 0 bus reset (SE0 now), bit 1 suspended, bits 3:2 line state, D+ and D- |
| `0x202`-`0x203` | `USB_FRAME` | RO | Last SOF frame number, 11 bits |
| `0x204` | `USB_IRQ` | W1C / RO | Bit 0 bus reset, 1 suspend, 2 resume, 3 SOF (W1C). Bit 5 any `SETUP_DEV`, 6 any `IN_DONE`, 7 any `OUT_DONE` (read-only summaries) |
| `0x205` | `USB_IRQ_EN` | RW | The same bits. An enabled event raises the USB bit in `IRQ_STATUS`, and STS bit 4 is high while one is set |
| `0x206` | `USB_SETUP_DEV` | W1C | Bit *d*: device *d* received a SETUP |
| `0x208`-`0x209` | `USB_IN_DONE` | W1C | Bit *k*: IN slot *k* was sent and acknowledged |
| `0x20A`-`0x20B` | `USB_OUT_DONE` | W1C | Bit *k*: OUT slot 16 + *k* received a packet |
| `0x210` | `MAP_ADDR` | WO | The USB address for `MAP_DEV` |
| `0x211` | `MAP_DEV` | WO | Queues address map [`MAP_ADDR`] = bit 7 valid, bits 2:0 device |
| `0x212` | `SETADDR_DEV` | WO | The device of a deferred SET_ADDRESS |
| `0x213` | `SETADDR_ADDR` | WO | Its new address. Writing it arms the deferral |
| `0x240` + *k* | `SLOT_CMD` | WO | Queues a command for slot *k* (0 to 31): bit 0 enable, 1 disable, 2 stall, 3 clear stall, 4 toggle DATA0, 5 toggle DATA1, 6 IN: disarm, OUT: buffer free |

Space 1:

| Address | Access | Contents |
|---|---|---|
| `0x000`-`0x3FF` | W | IN slot *k*'s data at *k* × 64 |
| `0x400`-`0x7FF` | R | OUT slot 16 + *k*'s data at `0x400` + *k* × 64 |
| `0x800`-`0x83F` | R | Device *d*'s SETUP packet at `0x800` + *d* × 8 |
| `0x840`-`0x84F` | R | Bytes received by OUT slot 16 + *k*, at `0x840` + *k* |
| `0xC00`-`0xCFF` | W | Endpoint map [device × 32 + endpoint × 2 + IN] = bit 7 valid, bits 3:0 slot within its half |
| `0xD00`-`0xD0F` | W | Queues: arm IN slot *k* with N bytes (0 to 64) |

What the hardware does, which the driver relies on:

- A token goes through the address map. With no valid entry it's ignored, with no handshake. Address 0 is an ordinary entry: map it to the hub, or to the device whose port the host is resetting.
- EP0 needs no endpoint map entry. Other endpoints need a valid entry and an enabled slot, or the token is ignored.
- A SETUP (exactly 8 bytes, DATA0) is always acknowledged into that device's SETUP buffer. It sets the device's `SETUP_DEV` bit and, for both its EP0 slots, clears stall and armed or full, and sets the toggle to DATA1.
- IN: stalled gets STALL, not armed gets NAK, armed sends DATAx. An ACK flips the toggle, disarms and sets `IN_DONE`. No ACK leaves it armed for the retry.
- OUT: stalled gets STALL, a full buffer gets NAK, otherwise it's received and acknowledged. A toggle mismatch (the host resending after a lost ACK) is acknowledged and dropped. Otherwise the buffer becomes full, the toggle flips, the length is stored and `OUT_DONE` is set. The driver reads the data, then frees the buffer with `SLOT_CMD` bit 6.
- A packet with a bad CRC, a bit-stuff error or more than 64 bytes gets no handshake.
- **Deferred SET_ADDRESS:** write `SETADDR_DEV`, then `SETADDR_ADDR`, then arm the status-stage ZLP on that device's EP0 IN. When the host acknowledges it, the address map gets the new address for that device. The old entry (address 0, for example) stays until the driver clears it.
- A bus reset (SE0 for 4µs or more) clears all slot state and the whole address map in about 11µs, and sets the bus reset bit. The endpoint map is kept.
- About 5ms with no bus activity sets suspend. Activity after that sets resume.

## Flash layout

On the UPduino's 4MB flash:

| Offset | Size | Contents |
|---|---|---|
| `0x000000` | 128KB | Bitstream |
| `0x022000` | 4KB | ROM catalogue, copy A |
| `0x023000` | 4KB | ROM catalogue, copy B |
| `0x024000` | 32KB | Retail tables |
| `0x040000` | 3.75MB | ROM slots: 15 × 256KB |

On a 16MB flash, the catalogue copies are at `0x042000` and `0x043000`, the tables at `0x044000`, and the ROM slots start at `0x100000` (60 slots). The ESP32 picks the layout from the flash's JEDEC size. The catalogue and tables are described in [ESP32](esp32.md#rom-library).
