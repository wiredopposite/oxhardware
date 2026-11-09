# 01 — ESP32 ↔ FPGA Interface Contract

Single source of truth for everything crossing the ESP32/FPGA boundary, plus the on-NOR boot config the FPGA parses. A change bumps `VERSION_MAJOR` (breaking) or `VERSION_MINOR` (additive). The register map is generated from `tools/regmap.yaml` into both `fpga/rtl/link/regmap.vh` and `esp32/components/fpga_link/regmap.h`.

## 1. Signals

### 1.1 Config bus (shared: FPGA config pins, QSPI NOR, ESP32 HSPI)

| Net | FPGA pin | NOR pin | ESP32 | Normal operation | ESP32 owns bus (FPGA in reset) |
|---|---|---|---|---|---|
| `CFG_SCK` | `SPI_SCK` | CLK | HSPI SCK | FPGA drives | ESP32 drives |
| `CFG_SS_N` | `SPI_SS_B` | CS# | GPIO | FPGA drives | ESP32 drives |
| `CFG_SO` (FPGA→NOR) | `SPI_SO` | DI/IO0 | HSPI MOSI | FPGA drives | ESP32 drives |
| `CFG_SI` (NOR→FPGA) | `SPI_SI` | DO/IO1 | HSPI MISO | NOR drives | NOR drives |
| `NOR_IO2`, `NOR_IO3` | user I/O | WP#/IO2, HOLD#/IO3 | — (pull-ups) | FPGA (quad) | Hi-Z, pulled up |

SO/SI are named from the FPGA's SPI-master view (UP5K-SG48 pins: SCK 15, SS 16, SO 14, SI 17). The ESP32 keeps all its config-bus pins **Hi-Z unless it is holding `CRESET_B` low**. Series resistors (~33 Ω) on the ESP32 side limit damage from a contention bug.

### 1.2 Link and control

| Signal | Dir (ESP32 view) | Notes |
|---|---|---|
| `LINK_SCK`, `LINK_MOSI`, `LINK_MISO`, `LINK_CS_N` | out, out, in, out | VSPI through the GPIO matrix. FPGA tri-states MISO while `LINK_CS_N` is high. Bus is dedicated to the FPGA (SD + LCD are on the ESP32's other SPI host). |
| `FPGA_IRQ_N` | in (pull-up) | Active-low level: `(IRQ_STATUS & IRQ_ENABLE) != 0` |
| `FPGA_CRESET_N` | out, open-drain | Low = FPGA held unconfigured, all its I/O Hi-Z with weak pull-ups |
| `FPGA_CDONE` | in | High = configured |

## 2. Boot modes

### 2.1 Normal (no ESP32 involvement)

Power-up → FPGA reads its bitstream from NOR offset 0 in SPI master mode → user design starts → reads the **boot config record** (§6) from NOR → if valid, sets bank/LPC/COM/iotrap registers from it and enables serving. The ESP32 boots in parallel, then connects over the link and takes over policy.

### 2.2 ESP32 programs the NOR (factory, update, recovery)

1. `CRESET_N = 0` (FPGA releases the config bus).
2. ESP32 drives the config bus as a plain SPI master: JEDEC ID `0x9F`, sector erase `0x20`/block erase `0xD8`, page program `0x02`, read `0x03`, status `0x05`.
3. ESP32 returns all config-bus pins to Hi-Z, `CRESET_N = 1` → FPGA reboots from NOR (normal mode).
4. Check `CDONE` within 200 ms; then `ID` over the link.

Used for: bitstream updates (write region, verify, reboot), unbricking (golden bitstream kept in ESP32 flash), and bulk ROM writes when the console is off (faster than through the FPGA). While the console is running, ROM writes go **through** the FPGA (§5.5) so LPC keeps being served.

### 2.3 ESP32 slave-configures the FPGA (fallback / development)

1. Put NOR in deep power-down (`0xB9`) via §2.2 so it ignores the bitstream.
2. `CS_N = 0`, `CRESET_N = 0` ≥ 200 ns, `CRESET_N = 1` with CS still low, wait for the device's configuration-memory clear time (Lattice FPGA-TN-02001; 1200 µs is safe).
3. CS high, 8 dummy clocks; CS low, send bitstream MSB-first; CS high, ≥ 100 dummy clocks; `CDONE` must be high, then ≥ 49 more clocks.
4. The design must release the NOR from power-down (`0xAB`) before reading boot config.

## 3. Link protocol

- SPI mode 0, MSB first, framed by `LINK_CS_N`. 20 MHz for bring-up, 26.67 MHz maximum (the ESP32-PICO-MINI-02 routes it through the GPIO matrix).
- FPGA shifts in the SCK domain, hands whole bytes to the 48 MHz system domain.

### 3.1 Transaction format

```
 write: MOSI | CMD | A[23:16] | A[15:8] | A[7:0] | data ...
        MISO | STS |  0x00    |  0x00   |  0x00  |
 read:  MOSI | CMD | A[23:16] | A[15:8] | A[7:0] | dummy × D | 0x00 ...
        MISO | STS |  0x00    |  0x00   |  0x00  | 0x00 × D  | data ...
```

**CMD**: bit7 `RD`; bits 6:4 `SPACE`; bit3 `NOINC` (no address increment); bits 2:0 reserved (0).

**STS** (free status on every transaction):

| Bit | Meaning |
|---|---|
| 7 | Any enabled IRQ pending |
| 6 | `LRESET#` level (1 = console running) |
| 5 | Serving active |
| 4 | USB IRQ pending |
| 3 | iotrap event FIFO non-empty |
| 2 | Mailbox / COM-to-ESP32 RX non-empty |
| 1 | `ERR_STATUS != 0` |
| 0 | NOR busy (erase/program in progress or suspended) |

**Dummy bytes `D`**: `SPACE` 0/1/3/4 → 1. `SPACE` 2 (NOR read window) → `LINK_NOR_DUMMY` (default 4); underrun sets `ERR_STATUS.NOR_UNDERRUN` (data garbage — check STS or use the CRC engine).

**Lengths**: unlimited in hardware; firmware uses ≤ 4 KB per transaction.

## 4. Address spaces

| SPACE | Contents |
|---|---|
| 0 | Registers (§5), 8-bit, `A[15:0]` |
| 1 | USB endpoint slot buffers: slot *k* at `k * 64` (k = 0..31), SETUP packets for device *d* at `0x800 + d*8` |
| 2 | NOR **read** window, byte address `A[23:0]` (16 MB). Writes are not allowed here — use §5.5. |
| 3 | FIFO ports (use `NOINC`): `0x00` COM (when routed to ESP32), `0x01` mailbox, `0x02` iotrap events (read-only, 4 bytes/event), `0x03` NOR program buffer (256 B) |
| 4–7 | Reserved |

## 5. Register map (SPACE 0)

8-bit registers; multi-byte little-endian, committed when the **highest-addressed byte** is written. `RO` read-only, `W1C` write-1-to-clear, `SC` self-clearing.

### 5.1 Identity & system — `0x0000`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x00–0x03 | `ID` | RO | `'O','X','F','G'` |
| 0x04–0x06 | `VERSION` | RO | major, minor, patch |
| 0x07 | `BOARD_ID` | RO | Pin-map variant |
| 0x08–0x09 | `FEATURES` | RO | bit0 LPC mem, 1 COM, 2 Super I/O, 3 mailbox, 4 USB, 5 CRC, 6 LFRAME abort, 7 iotrap, 8 flash emu, 9 reserved, 10 bank map, 11 quad NOR (0 = single/dual-SPI board, e.g. UPduino) |
| 0x0A | `BOOT_STATUS` | RO | bit0 boot config valid, bit1 serving from boot config, bit2 config CRC bad (defaults used), bit3 woke via slave config |
| 0x0C–0x0F | `SCRATCH` | RW | Link test |
| 0x10–0x11 | `IRQ_STATUS` | W1C | bit0 `LRST_ASSERT`, 1 `LRST_DEASSERT`, 2 `BANK_SWAPPED`, 3 `USB`, 4 `IOTRAP` (FIFO above threshold or deferred read pending), 5 `MBX_RX`, 6 `COM_RX`, 7 `ERROR`, 8 `CRC_DONE`, 9 `NOR_DONE`, 10 `FLASHEMU` (Xbox started/finished a flash write sequence), 11 reserved |
| 0x12–0x13 | `IRQ_ENABLE` | RW | Same bits |
| 0x14 | `ERR_STATUS` | W1C | bit0 `NOR_UNDERRUN`, 1 `FIFO_UNDERRUN`, 2 `FIFO_OVERFLOW`, 3 `LPC_SYNC_TIMEOUT`, 4 `LINK_BAD_CMD`, 5 `IOTRAP_OVERFLOW`, 6 `FLASHEMU_PROTECT` (write to protected bank refused) |
| 0x15 | `SOFT_RESET` | SC | `0xA5`: reset all blocks except link |

### 5.2 LPC — `0x0020`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x20 | `LPC_CTRL` | RW | bit0 `SERVE_EN`; bit1 `D0_EN`; bit2 `LFRAME_ABORT`; bit3 `ARM_AT_RESET` (SC); bit4 `IO_EN`; bit5 `PAUSE` (mem reads short-wait until cleared/timeout); bit6 `START_FROM_LAD_ONLY` |
| 0x21 | `LPC_STATUS` | RO | bit0 `LRESET#`; bit1 LCLK running; bit2 served ≥ 1 read since reset; bit3 swap armed |
| 0x22 | `ACTIVE_BANK` | RW | bits 5:0 `SLOT` (256 KB units from `ROM_BASE`), bits 7:6 `SIZE` (0 = 256 K, 1 = 512 K, 2 = 1 M). Slot aligned to size. |
| 0x23 | `PENDING_BANK` | RW | Same; applied by `ARM_AT_RESET` |
| 0x24 | `LPC_RESET_COUNT` | RO | Wrapping |
| 0x25 | `LPC_TIMEOUT_COUNT` | RO | Saturating |
| 0x26 | `SYNC_TIMEOUT` | RW | Units of 16 LCLK, default 0x80 (≈ 61 µs) |
| 0x28–0x2A | `LAST_MEM_ADDR` | RO | `A[23:0]` of last served read |
| 0x2C–0x2F | `MEM_READ_COUNT` | RO | Since reset |
| 0x30–0x37 | `PROTECT_MASK` | RW | 64-bit mask, one bit per 256 KB slot: flash emulation may not erase/program it |

NOR address served = `ROM_BASE + ACTIVE_BANK.SLOT * 256 KB + (A[19:0] & size_mask)` for every memory read with `A[31:24] = 0xFF` (`ROM_BASE` from the boot config, §6).

### 5.3 COM / UART / Super I/O — `0x0040`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x40 | `COM_CTRL` | RW | bit0 `COM_EN`; bit1 `ROUTE` (0 = UART pins, 1 = ESP32); bit2 `RTSCTS` (hardware flow control on the physical UART); bit3 `SUPERIO_EN`; bit4 `SUPERIO_4E`; bit5 `UART_LOOPBACK` |
| 0x41–0x42 | `COM_BASE` | RW | Default 0x03F8; overridden by Super I/O LDN 4 when the guest activates it |
| 0x43–0x44 | `UART_DIV` | RW | Baud = 48 MHz / `UART_DIV`; default 417 (115 200). 52 ≈ 921 600, 16 = 3 M. Guest DLL/DLM never affect it. |
| 0x45 | `COM_TX_LEVEL` | RO | Xbox→out bytes queued (sat. 255) |
| 0x46 | `COM_RX_LEVEL` | RO | in→Xbox bytes queued |
| 0x47 | `COM_STATUS` | RO | bit0 guest activated COM via Super I/O; bit1 RX overflow seen; bit2 guest DLL/DLM last written (debug: `0x48–0x49`) |
| 0x48–0x49 | `COM_GUEST_DIV` | RO | Last guest divisor (informational) |

### 5.4 iotrap (legacy I/O) — `0x0060`

Eight windows, 8 bytes each at `0x60 + 8w`:

| Offset | Name | Description |
|---|---|---|
| +0..+1 | `TRAP_BASE` | 16-bit I/O base (aligned to size) |
| +2 | `TRAP_SIZE` | log2 ports, 0–4 (1–16 ports) |
| +3 | `TRAP_MODE` | bit0 enable; bit1 capture writes to event FIFO; bit2 writes also update shadow; bit3 reads from shadow; bit4 reads deferred to ESP32 (long-wait SYNC until answered or timeout → 0xFF); bit5 capture reads as events; bit6 write back-pressure (short-wait SYNC while FIFO full); bit7 bank-select fast path (written value → `BANKMAP` → `ACTIVE_BANK`/`PENDING_BANK`) |
| +4 | `TRAP_SHADOW_IDX` | Shadow RAM offset for this window (16 B granules) |
| +5 | `TRAP_BANKSEL` | bit0 apply at reset vs. immediately; bits 7:4 mask applied to the written value before lookup |

| Addr | Name | Access | Description |
|---|---|---|---|
| 0xA0 | `TRAP_FIFO_LEVEL_LO/HI` (0xA0–0xA1) | RO | Events queued (0–8191) |
| 0xA2–0xA3 | `TRAP_FIFO_THRESH` | RW | IRQ threshold |
| 0xA4 | `TRAP_DEFER_STATUS` | RO | bit0 a deferred read is pending; port in `0xA6–0xA7` |
| 0xA5 | `TRAP_DEFER_REPLY` | W | Value to return; completes the pending cycle |
| 0xA6–0xA7 | `TRAP_DEFER_PORT` | RO | |
| 0xB0–0xBF | `BANKMAP` | RW | 16 entries, `ACTIVE_BANK` format, indexed by the (masked) written nibble |

Shadow RAM (128 B) is SPACE 0 `0x0400–0x047F`.

Event record (SPACE 3 port 2, 4 bytes, little-endian): `port[15:0]`, `data[7:0]`, `flags[7:0]` = bit0 read (1) / write (0), bits 3:1 window, bit4 FIFO overflowed before this event, bits 7:5 coarse Δt since previous event (0 < 1 µs, 1 < 4 µs, … 7 ≥ 4 ms) — enough to separate bit-banged bursts.

### 5.5 NOR / flash emulation / CRC — `0x00C0`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0xC0 | `NOR_STATUS` | RO | bit0 init done; bit1 quad enabled; bit2 busy; bit3 suspended (LPC read pre-empted an erase/program); bit4 CRC busy |
| 0xC1 | `LINK_NOR_DUMMY` | RW | Default 4 |
| 0xC2–0xC4 | `NOR_JEDEC_ID` | RO | Captured at init |
| 0xC8–0xCA | `NOR_OP_ADDR` | RW | |
| 0xCB | `NOR_OP` | SC | 1 = erase 4 K, 2 = erase 64 K, 3 = program from SPACE 3 port 3 buffer (≤ 256 B, page-aligned), 4 = write status/config regs; raises `NOR_DONE`. Refused (error) for the bitstream region unless `NOR_UNLOCK = 0x5A`. |
| 0xCC | `NOR_PROG_LEN` | RW | Bytes in the program buffer − 1 |
| 0xCD | `NOR_UNLOCK` | RW | `0x5A` permits ops below `ROM_BASE` (bitstream/config); clears after one op |
| 0xD0–0xD2 | `CRC_ADDR` | RW | |
| 0xD3–0xD5 | `CRC_LEN` | RW | |
| 0xD6 | `CRC_CTRL` | SC | bit0 start → `CRC_DONE` |
| 0xD8–0xDB | `CRC_RESULT` | RO | CRC-32 IEEE (zlib / `esp_rom_crc32_le` compatible) |
| 0xE0 | `FE_CTRL` | RW | bit0 enable JEDEC flash emulation; bit1 chip-erase maps to active bank only (always 1 — read-only) |
| 0xE1–0xE2 | `FE_ID` | RW | Manufacturer / device ID returned in autoselect mode (from boot config) |
| 0xE3 | `FE_STATUS` | RO | bit0 Xbox in a program/erase sequence; bit1 last op refused (protected); bits 7:4 last command type |
| 0xE4–0xE6 | `FE_LAST_ADDR` | RO | |

### 5.6 Mailbox — `0x0100`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x100 | `MBX_CTRL` | RW | bit0 enable; bit1 `RESET_REQ` (W1C) |
| 0x101–0x102 | `MBX_BASE` | RW | I/O base of the 4-port mailbox (layout per 00-system Q5) |
| 0x103–0x104 | `MBX_RX_LEVEL` | RO | |
| 0x105–0x106 | `MBX_TX_FREE` | RO | |

### 5.7 LED — `0x0110`

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x110 | `LED_CTRL` | RW | bit0 auto (FPGA status) |
| 0x111–0x113 | `LED_RGB` | RW | Manual PWM duty |

### 5.8 Reserved — `0x0120`

Reserved.

### 5.9 USB device — `0x0200`

Devices (logical USB devices sharing the port — composite = 1 device; hub emulation = hub + downstream), at `0x210 + 4d`, d = 0..3:

| Offset | Name | Description |
|---|---|---|
| +0 | `DEV_CTRL` | bit0 enable; bit1 answers at address 0 (only one device should have this set at a time — the hub, or the downstream device whose port the host is resetting) |
| +1 | `DEV_ADDR` | bits 6:0 address; bit7 `DEFER` (apply after the next EP0-IN ZLP of this device completes) |

Global:

| Addr | Name | Access | Description |
|---|---|---|---|
| 0x200 | `USB_CTRL` | RW | bit0 `PULLUP`; bit1 `SIE_EN`; bit2 `FORCE_RESUME` |
| 0x202 | `USB_STATUS` | RO | bit0 bus reset (SE0); bit1 suspended |
| 0x203–0x204 | `USB_FRAME` | RO | Last SOF frame number |
| 0x205 | `USB_IRQ` | W1C | bit0 `BUS_RESET`, 1 `SETUP` (see `USB_SETUP_DEV`), 2 `IN_DONE`, 3 `OUT_DONE`, 4 `SUSPEND`, 5 `RESUME`, 6 `SOF` |
| 0x206 | `USB_IRQ_EN` | RW | |
| 0x207 | `USB_SETUP_DEV` | W1C | Bitmap: device *d* received a SETUP |
| 0x208–0x20B | `USB_IN_DONE` | W1C | Bitmap by slot |
| 0x20C–0x20F | `USB_OUT_DONE` | W1C | Bitmap by slot |

Endpoint slots (k = 0..31; slot 2d and 2d+1 are reserved as device *d*'s EP0 IN/OUT), at `0x240 + 4k`:

| Offset | Name | Description |
|---|---|---|
| +0 | `SLOT_CFG` | bit0 enable; bit1 STALL; bit2 direction (1 = IN); bits 4:3 type (0 ctrl, 2 bulk, 3 int); bits 6:5 device; bit7 data toggle (next IN / expected OUT; FPGA updates) |
| +1 | `SLOT_EP` | bits 3:0 endpoint number |
| +2 | `SLOT_LEN` | IN: write = arm with N bytes (0–64, 0 = ZLP). OUT: bytes received (RO) |
| +3 | `SLOT_CMD` | IN: read bit0 armed. OUT: write 1 = buffer free, ACK next OUT |

Hardware behaviour the DCD relies on:

- Token → find the enabled device whose address matches (or with "answers at 0" for address 0); no match → ignore silently (no handshake).
- Then find the enabled slot with (device, EP number, direction); none → ignore (no handshake).
- **SETUP always ACKed** into that device's SETUP buffer; clears its EP0 STALLs, disarms EP0 IN, sets EP0 toggles to 1, sets `USB_SETUP_DEV[d]`.
- IN on unarmed slot → NAK. Armed → DATAx; ACK → flip toggle, disarm, `IN_DONE[k]`; no ACK → stay armed.
- OUT with buffer busy → NAK; free → receive, CRC/toggle check (mismatch = ACK + drop), ACK, `OUT_DONE[k]`.
- STALL bit → STALL. Bus reset → all devices address 0, non-EP0 slots disabled, buffers disarmed, `BUS_RESET`.

## 6. Boot config record (NOR)

### 6.1 NOR layout (16 MB)

| Offset | Size | Contents |
|---|---|---|
| 0x000000 | 128 KB | Bitstream (cold-boot image) |
| 0x020000 | 128 KB | Bitstream B (update staging / future warm-boot image) |
| 0x040000 | 4 KB | Boot config record A |
| 0x041000 | 4 KB | Boot config record B (written alternately; highest valid sequence wins) |
| 0x042000 | 248 KB | ROM catalogue + ESP32-owned metadata (format owned by ESP32 firmware) |
| 0x080000 | 512 KB | Reserved (logs, personality data) |
| **0x100000** (`ROM_BASE`) | 15 MB | ROM banks: 60 × 256 KB slots |

**Small-flash layout (≤ 4 MB, e.g. the UPduino v3.1 dev board):**

| Offset | Size | Contents |
|---|---|---|
| 0x000000 | 128 KB | Bitstream |
| 0x020000 | 4 KB | Boot config record A |
| 0x021000 | 4 KB | Boot config record B |
| 0x022000 | 120 KB | ROM catalogue + metadata |
| **0x040000** (`ROM_BASE`) | 3.75 MB | ROM banks: 15 × 256 KB slots |

The boot-config address is a per-board build parameter (`BOOT_CFG_ADDR`); `ROM_BASE` comes from the record (§6.2), so one RTL serves both layouts. No bitstream-B slot on small parts — updates are written in place, with the ESP32's golden image as the safety net.

### 6.2 Record format (parsed by the FPGA at boot)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `'O','X','B','C'` |
| 4 | 1 | Version (1) |
| 5 | 1 | Length of record in bytes |
| 6 | 2 | Sequence number (wrapping, newer wins) |
| 8 | 1 | `LPC_CTRL` initial value (`SERVE_EN`, `D0_EN`, `LFRAME_ABORT`, `IO_EN`) |
| 9 | 1 | `ACTIVE_BANK` boot bank |
| 10 | 8 | `PROTECT_MASK` |
| 18 | 1 | `COM_CTRL` |
| 19 | 2 | `COM_BASE` |
| 21 | 2 | `UART_DIV` |
| 23 | 2 | `FE_ID` |
| 25 | 1 | `FE_CTRL` |
| 26 | 1 | `LED_CTRL` |
| 27 | 2 | `ROM_BASE` in 64 KB units (`0x0010` = 0x100000, `0x0004` = 0x040000) |
| 29 | 1 | iotrap window count *n* (0–8) |
| 30 | 8·*n* | iotrap window registers (§5.4 layout) |
| … | 16 | `BANKMAP` |
| … | 128 | Shadow RAM initial contents |
| end − 4 | 4 | CRC-32 of all preceding bytes |

Invalid/missing record → safe defaults: `SERVE_EN` off (stock TSOP boot on 1.0–1.5), COM on at 0x3F8 / 115200, iotrap off, LED shows "unconfigured".

## 7. Firmware-side rules

- IRQ handling: one burst read of `IRQ_STATUS` (+ USB bitmaps when flagged), service, W1C what was handled.
- Prefer `PENDING_BANK` + `ARM_AT_RESET` for switches while the console runs.
- After any ROM write, verify with the CRC engine before marking the catalogue entry valid.
- Writing the boot config: write the *other* record slot with sequence+1, verify CRC, never erase the current one first.
- Check `VERSION_MAJOR`; on mismatch, don't touch LPC registers (the FPGA is already serving from boot config) and offer a bitstream update.
