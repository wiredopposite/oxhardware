# 02 — FPGA Gateware Specification (iCE40UP5K)

## 1. Device budget

| Resource | UP5K | Planned use |
|---|---|---|
| LUT4 / FF | 5280 | ~4.0–4.6 k estimated (§10) — tight; features are build-time parameters |
| EBR (512 B each) | 30 | USB buffers + tables (8), COM FIFOs (2), mailbox (2), NOR prefetch + program buffer (2), shadow RAM + link FIFOs (2) |
| SPRAM (32 KB each) | 4 | 1 × iotrap event FIFO (8 K × 32 bit); others spare (possible ROM cache) |
| PLL | 1 | 12 MHz → 48 MHz |
| LFOSC | 10 kHz | Power-on timers, LCLK-present watchdog |
| RGB driver | 1 | Status LED |
| I/O (SG48) | 39 incl. 3 RGB | ~32 used, see 04-hardware (UPduino exposes 32 on headers) |

## 2. Clock domains

| Domain | Source | Freq | Contents |
|---|---|---|---|
| `clk_lpc` | `LCLK` on a global-buffer input | 33.33 MHz, stops when console off | LPC FSM, guest-facing register files (16550, Super I/O, mailbox, iotrap decode) |
| `clk_sys` | PLL from 12 MHz oscillator | 48 MHz | Link back-end, regfile, NOR controller, flash emulation, CRC, UART PHY, USB, LED |
| `clk_sck` | `LINK_SCK` | ≤ 20 MHz | Link shift registers |

Crossings (`rtl/common/`): toggle req/ack for LPC memory reads/writes (one outstanding, tagged so late data for an abandoned cycle is dropped); dual-clock EBR FIFOs for COM/mailbox/iotrap events; quasi-static config 2-FF synchronised; `LRESET#` synchronised into both domains; bank registers live in `clk_sys`. iotrap shadow RAM is a dual-port EBR (write: sys, read: lpc). Deferred reads use the same handshake as memory reads.

**Boot**: after configuration, `clk_sys` comes up from the PLL (lock ~ tens of µs); a sys-domain boot sequencer releases the NOR from power-down, enables quad mode, reads the boot config record, then enables LPC. LPC FSM stays off the bus until the sequencer says ready.

## 3. Module hierarchy

```
top_<board>.v              pins, SB_IO, SB_PLL40_PAD, SB_RGBA_DRV, SB_SPRAM256KA
└── openox_core.v
    ├── boot/boot_seq.v           NOR wake, boot-config parse + CRC, register init
    ├── link/{spi_slave,link_ctrl,regfile}.v
    ├── lpc/lpc_target.v          START/CYCTYPE/ADDR/TAR/SYNC/DATA, mem read+write, I/O read+write, abort, timeouts, LFRAME#, D0
    ├── lpc/lpc_io_bus.v          claims I/O cycles for: com, superio, mailbox, iotrap (exact decode, priority order)
    ├── lpc/com16550.v, lpc/superio_cfg.v, lpc/mailbox.v
    ├── lpc/iotrap.v              8 windows, shadow RAM, event FIFO writer, deferred reads, bank-select fast path
    ├── mem/nor_qspi.v            W25Q-class: read (quad I/O + continuous mode), erase/program, suspend/resume
    ├── mem/nor_arbiter.v         LPC read > flash-emu > link read > link program/erase > CRC
    ├── mem/rom_prefetch.v        32-byte line buffer(s)
    ├── mem/flash_emu.v           JEDEC command FSM on LPC memory writes
    ├── mem/crc32_engine.v
    ├── uart/uart_phy.v           8N1, RTS/CTS optional
    ├── usb/{usb_phy_rx,usb_phy_tx,usb_sie,usb_core}.v
    └── led/status_led.v
```

Build-time parameters (`FEATURE_*`) let a smaller bitstream drop iotrap/flash-emu/Super I/O if LUTs run out. `NOR_WIDTH` (1/2/4) selects single, dual-output (`0x3B`) or quad (`0xEB`) reads per board.

Vendor primitives (`SB_IO`, `SB_PLL40_PAD`, `SB_RGBA_DRV`, `SB_SPRAM256KA`) appear only in `boards/` and thin wrappers in `rtl/common/prim_*.v`, so a fallback FPGA (§10.1) means new wrappers, not a rewrite.

## 4. LPC target

### 4.1 Cycle formats

| Clock | 0 | 1 | 2–9 | 10–11 | 12.. | +1,+2 | +3,+4 |
|---|---|---|---|---|---|---|---|
| **Mem read** | START | `010x` | A31..A0 | TAR | SYNC | D[3:0], D[7:4] | TAR |
| **Mem write** | START | `011x` | A31..A0 | D[3:0], D[7:4] (10–11) | TAR (12–13) | SYNC (14) | TAR (15–16) |

| Clock | 0 | 1 | 2–5 | 6–7 | 8 | 9–10 | 11–12 |
|---|---|---|---|---|---|---|---|
| **I/O read** | START | `000x` | A15..A0 | TAR | SYNC | D[3:0], D[7:4] | TAR |
| **I/O write** | START | `001x` | A15..A0 | D[3:0], D[7:4] | TAR (8–9) | SYNC (10) | TAR (11–12) |

### 4.2 FSM rules

- **START** = `LFRAME#` low and `LAD = 0000`; a new START aborts any cycle in progress. Option `START_FROM_LAD_ONLY` hunts for `LAD = 0000` only between cycles, tracking each cycle to its end (TAR/SYNC/data/TAR, or SYNC timeout on `1111`) so a `0000` SYNC/data nibble isn't mistaken for START. **Required on 1.3–1.5**: those boards' LPC header has no LFRAME# (xboxdevwiki LPC Debug Port) unless it is reconnected with a jumper wire. Selected per board from the boot config (`LPC_CTRL`).
- **Memory read**: claim if `SERVE_EN && A[31:24] == 0xFF`. While `flash_emu` is in autoselect/status mode, the read is answered by `flash_emu` instead of NOR data.
- **Memory write**: claim if `FE_CTRL.enable && A[31:24] == 0xFF`; pass `{A[19:0], D}` to `flash_emu`. Not enabled → not claimed (stay off the bus).
- **I/O**: exact 16-bit decode per device; unclaimed cycles are not answered.
- **SYNC**: short wait `0101` until ready, then `0000`. `SYNC_TIMEOUT` → `1010` once, TAR, release, count, `ERROR`. Deferred iotrap reads and back-pressured writes use the same timeout.
- **TAR**: drive `1111` on the first TAR clock after our SYNC/data, then tri-state.
- **Reset**: `LRESET#` (filtered: must stay low 16 LCLK) low → FSM idle, LAD released (LFRAME# boot hold §4.4 and D0 §4.5 are not), 16550/Super I/O/mailbox state reset. iotrap shadow RAM is *not* reset (ESP32-owned state).

### 4.3 Address → NOR

`nor_addr = ROM_BASE + {ACTIVE_BANK.SLOT, 18'b0} + (A[19:0] & size_mask)`. Mirrors 256/512 K images through the 1 MB window and the window through all of `0xFF000000–0xFFFFFFFF`.

### 4.4 1.6 — LFRAME# abort

On a 1.6 the MCPX's onboard flash (behind the Xyclops) answers every memory read, so a claimed read must be hidden from it by holding `LFRAME#` low: a target takes START from the last clock on which `LFRAME#` is low, so while `LFRAME#` stays low the other device keeps waiting for START and never decodes our cycle.

- **Abort window**: once a memory read is claimed (`A31..24` known at clock 3), pull `LFRAME#` low from the address phase through the second TAR clock, then release. Asserting it during CYCTYPE would look like a new START. The last clock with `LFRAME#` low has `LAD = 1111`, so it is not a START either.
- **Boot hold**: from `LRESET#` until the read of `FF000074` has been served, `LFRAME#` is held low continuously, then per-cycle aborts take over. The first fetches then don't depend on how fast the switch turns on and off.
- **Drive**: an iCE40 pad cannot pull `LFRAME#` low against the MCPX's driver. It switches a logic-level N-MOSFET (e.g. IRLML6344): gate from the FPGA with 10 kΩ gate-to-source, source to the console's GND at the header, drain to `LFRAME#`. The FPGA reads `LFRAME#` on a separate input. Measured turn-off leaves `LFRAME#` low for ~2 LCLK after release, which the MCPX tolerates.
- **Not claimed**: memory reads outside `FFxxxxxx` (counted only).

### 4.5 D0 (1.0–1.5)

`D0_N` open-drain low whenever `SERVE_EN && D0_EN`, **including during `LRESET#`**; otherwise Hi-Z → TSOP. Unconfigured FPGA = Hi-Z, so a failed/slow boot falls back to TSOP. Self-boot (~20–70 ms) must assert D0 before the MCPX's first read (≥ 78 ms after power, 00-system §4.4).

### 4.6 Read latency & prefetch

NOR quad I/O fast read `0xEB` in continuous-read mode (no command byte after the first): 6 address + 2 mode + 4 dummy + 2 data = 14 clocks at 48 MHz ≈ 290 ns + CDC ≈ 13–15 LCLK per uncached byte (~12 waits). `rom_prefetch` fetches the aligned 32-byte line on a miss (~1 µs) and serves hits with zero waits; the MCPX reads sequentially, so most reads hit. Invalidate on bank change, flash-emu write, or link program/erase. SPRAM cache or a faster NOR clock (72–96 MHz) are later options.

### 4.7 NOR busy vs. LPC reads

A single NOR can't be read during erase/program. If an LPC read arrives while busy, `nor_qspi` issues **suspend** (`0x75`, tSUS ≤ 20 µs on the W25Q128JV), serves the read (SYNC waits; well under the 61 µs timeout), and **resumes** (`0x7A`) after the bus has been idle ~10 µs. Flash-emulation operations are exempt — their status reads are answered by `flash_emu` per JEDEC (§6).

## 5. COM port

### 5.1 16550 model

- Registers at `COM_BASE+0..7`: RBR/THR, IER, IIR/FCR, LCR, MCR, LSR, MSR, SCR; DLL/DLM with DLAB. Reset values `DLL=1`, `LCR=0x03`.
- **Guest baud is ignored**: DLL/DLM stored and read back only (also exposed as `COM_GUEST_DIV`). Physical baud comes from `UART_DIV`.
- **Back-pressure**: LSR.THRE|TEMT (0x60) only while the TX FIFO has room — a polled sender waits instead of dropping. LSR.DR = RX non-empty.
- IIR = 0x01 (no interrupt) | 0xC0 if FCR enabled FIFOs. IER stored (masked 0x0F). MSR = 0xB0 (DCD|DSR|CTS). MCR stored (0x1F), loopback not implemented. FCR bit1 clears RX. SERIRQ not implemented.
- On `LRESET#`: registers to defaults, RX FIFO drained; the TX FIFO keeps draining to the UART so output from just before a reboot is not lost.

### 5.2 Physical side

`uart_phy` 8N1 at `48 MHz / UART_DIV` to bridge channel B. With `RTSCTS`: deassert RTS when RX FIFO > 3/4 full; stop TX while CTS deasserted. FIFOs 512 B each. `ROUTE=1` connects the FIFOs to link SPACE 3 port 0 instead.

### 5.3 Super I/O

SMSC LPC47M-style: 0x55 enter / 0xAA exit at 0x2E (or 0x4E); 0x07 LDN; per-LDN 0x30 activate, 0x60/0x61 base. LDN 4 activation overrides `COM_BASE`. Registers 0x26/0x27 hold the chip's own config port, so 0x26 reads back 0x2E and 0x27 reads 0x00. The data port (0x2F) answers only in config mode (after 0x55, until 0xAA).

The devkit kernel probes for this at boot (§6.2: write 0x55, select 0x26, read 0x2F) and enables serial debug output and the KD stub only if it answers. Test: boot a devkit BIOS and expect its debug output on the physical UART.

## 6. Legacy modchip support

### 6.1 iotrap

Eight windows (`TRAP_*`, 01-interface §5.4). Per guest I/O cycle that hits a window:

| Mode bit | Write cycle | Read cycle |
|---|---|---|
| capture | Push event `{port, data, flags, Δt}` to the SPRAM FIFO | (if "capture reads") push event |
| shadow | Also store `data` in shadow RAM | Return shadow byte (immediate SYNC ready) |
| deferred | — | Long/short-wait SYNC, raise `IOTRAP`, ESP32 writes `TRAP_DEFER_REPLY`; timeout → 0xFF |
| back-pressure | Short-wait SYNC while FIFO full (bounded by `SYNC_TIMEOUT`, then drop + overflow flag) | — |
| bank-select | `BANKMAP[data & mask]` → `ACTIVE_BANK` (immediate) or `PENDING_BANK` + arm (at reset) | — |

Rationale: legacy LCD/LED interfaces are write-mostly (HD44780 over parallel-ish I/O ports, bit-banged SPI for Xenium-style LCDs) — writes must be captured in order, never lost, never slowed down. Reads are mostly status/ID bytes the ESP32 can keep in shadow RAM. The event FIFO (8 K entries) absorbs bursts and anything written before the ESP32 has booted.

### 6.2 Personalities (ESP32-side, examples)

| Chip | I/O ports | iotrap use |
|---|---|---|
| Xenium | **Observed** (see below): `0xEE` LED, `0xEF` bank select + bit-banged serial (LCD) + status read | `0xEE` capture + shadow; `0xEF` bank-select fast path + capture (serial bits) + shadow read |
| SmartXX | LCD data/command ports in the `0xF7xx` range, LED port; not yet characterised | capture + shadow |
| Xecuter 3 | LCD ports in the `0xF5xx`/`0xF7xx` range; not yet characterised | capture + shadow |
| Aladdin / others | LCD ports; not yet characterised | capture + shadow |

**Xenium-compatible chip running XeniumOS, observed with `lpc_iolog`** (1.6, chip on an LPC splitter): power on → XeniumOS menu (~2 min) → boot a BIOS from bank 3.

| Port | Observed | Meaning (inferred) |
|---|---|---|
| `0xEE` W | 21 writes, values `00`–`07` only. At boot it steps `00,01,…,07` within ~26 ms (LED self-test), then e.g. `02`,`03`,`07`,`01`; `00` written just before the final reset | 3-bit LED colour |
| `0xEF` W `[3:0]` | Values `1`, `2`, `3`, `A`. `2` at 620 ms, then `A`/`1` alternate repeatedly while the menu runs; `3` = the bank booted | Bank select. XeniumOS switches banks itself at runtime (presumably to read its BIOS list/settings from another bank), so the fast path must apply immediately, not only at reset |
| `0xEF` W `[6:4]` | Idle `111` (`0x70`). Bursts of 131 / 520 / 286 writes in which bit 6 toggles every write pair and bit 4 carries data (patterns `4x`/`5x` vs `0x`/`1x`); no reads of `0xEF` during bursts | Write-only bit-banged serial: bit 6 = clock, bit 4 = data, bit 5 = select (low during bursts). Consistent with an SPI LCD. Each bit costs ~3 LPC I/O writes (~46 µs/bit as observed) |
| `0xEF` W `[7]` | Set exactly once: `73` → `F3`, then `LRESET#` asserted 1.06 ms later | Triggers boot of the selected bank; the reset follows |
| `0xEF` R | Read once at 2.05 s → `BA` | Chip detect / status byte; serve `BA` from shadow RAM |
| `0x3F8–0x3FD` | XeniumOS (RTTarget-32) programs COM1 (divisor 1 = 115200 8N1, MCR 03) and prints its banner `RTTarget-32 4.0 32-Bit Boot Code …`; LSR (`0x3FD`) reads get no response (`FF`) | Our 16550 will receive this; confirms F-COM is used by real software on boot |
| `0x2E/0x2F` | After the booted BIOS (a **devkit BIOS** with serial debug output and a WinDbg/KD stub) starts: write `55`, `26`, read `2F` → no response, and no COM traffic follows | Kernel Super I/O probe: SMSC enter-config key `55`, then config index `0x26`. With nothing answering, the kernel concludes there is no Super I/O and skips serial debug/KD. Answered by F-COM-4 (§5.3) |
| `LRESET#` | After `F3`: asserted 1.06 ms, released 1.1 ms later, asserted again 21 ms later, released 21 ms after that | Two reset pulses on a bank-switch reboot; the bank written before the reset must persist through both |

Memory reads aren't logged by `lpc_iolog`, so which device served the boot (and from which bank) isn't directly visible.

### 6.3 JEDEC flash emulation (`flash_emu`)

Lets existing Xbox flashers (which talk to a parallel/LPC flash chip with AMD/SST-style command sequences through memory cycles at `0xFFxxxxxx`) write ROMs into the active bank.

- Address decode uses `A[19:0] & size_mask` (unlock addresses 0x5555/0x2AAA or 0x555/0x2AA, depending on the emulated part).
- Commands: reset (`F0`), autoselect (`90` → reads return `FE_ID` mfr/device at offsets 0/1), byte program (`A0`), sector erase (`80`,`30`), chip erase (`80`,`10` → **active bank only**), status polling: DQ7 data-polling and DQ6 toggle while busy.
- Byte program → buffered into the NOR program buffer; flushed as a page program when the next byte leaves the page or after ~50 µs idle (status polling reports "busy" until flushed).
- Sector size presented to the flasher is 4 KB, matching both the SST49LF0x0 parts and the NOR's smallest erase.
- `PROTECT_MASK` slot → command ignored, `FLASHEMU_PROTECT` error, status returns "done" (flashers then fail verify, safely).
- Raises `FLASHEMU` on sequence start/finish so the ESP32 can update the catalogue (CRC, "modified by console").

## 7. NOR controller

Target part: W25Q128JV-class (16 MB, 3.3 V, quad, 4 KB sectors, erase/program suspend).

- Boot: release power-down `0xAB`, set QE (status reg 2) once at factory, quad I/O read `0xEB` with continuous mode.
- Ops: 4 KB erase `0x20`, 64 KB erase `0xD8`, page program `0x02` (or quad `0x32`), status `0x05/0x35`, suspend `0x75`, resume `0x7A`.
- Region guard: anything below `ROM_BASE` refused unless `NOR_UNLOCK`.
- SCLK = 48 MHz via DDR output register; capture on a delayed/opposite edge — validate margins in nextpnr and on hardware.
- **UPduino v3.1**: W25Q32 (4 MB, JEDEC `EF 40 16`) on the config pins: SCK 15, CS# 16, SO 14 → DI, SI 17 ← DO. IO2/IO3 are not connected to the FPGA, so the board builds with `NOR_WIDTH=1`. `lpc_serve` reads with `0x03` at SCK = LCLK/2 into a two-byte stream buffer; sequential fetches then cost a few SYNC waits each and the worst observed wait was 87 LCLK (00-system §4.4). Small-flash layout per 01-interface §6.1.

## 8. ESP32 link

Implements 01-interface §3. MISO tri-stated while `LINK_CS_N` high. `IRQ_N` push-pull active-low. Link is not reset by `SOFT_RESET`.

## 9. USB full-speed device

### 9.1 Electrical

`USB_DP`, `USB_DN` (bidirectional `SB_IO`), `USB_PU` (1.5 kΩ to D+; Hi-Z = detached), ~27 Ω series on D+/D−. Connects to the MCPX controller-port test pads.

### 9.2 Architecture

```
 D+/D- ─► sample (48 MHz, 4×) ─► NRZI dec ─► decide ─► unstuff, bytes, CRC5/16 ─► events ─┐
                                                                                         │ toggle
 usb_sie (24 MHz): token ─► address map ─► endpoint map ─► slot state + IN length ─► handshake
                                                                                         │ toggle
 D+/D- ◄─ NRZI enc ◄─ stuff ◄─ SYNC, PID, data, CRC16, EOP (48 MHz) ◄── bytes ───────────┘
```

- **PHY** (`usb_phy_rx`, `usb_phy_tx`, 48 MHz): the receiver resynchronises on every transition and samples two clocks later. NRZI decode, a decision stage, then unstuffing, byte assembly and both CRCs serially. Counters are one-hot and the transmitter decides each bit boundary one clock early, to keep 48 MHz paths at one or two LUT levels. Events (packet start, byte, end with CRC flags) and transmit bytes cross to the SIE with toggles.
- **SIE** (`usb_sie`, system clock): lookups instead of compares. Address map and slot state share one block RAM owned by the engine; the endpoint map and IN lengths are another, written by the link. The address map is read as soon as the token's address byte is in, so the handshake is ready when the EOP arrives. Measured turnaround 4.5 bit times (limit 7.5).
- **Buffers**: IN 16 × 64 B (link writes, SIE reads); OUT 16 × 64 B + SETUP 8 × 8 B + lengths (SIE writes, link reads). One writer and one reader per RAM, so neither side waits.
- **Queue**: slot commands, arms and address-map writes from the link go into a 256-entry queue that runs between transactions, so state never changes under an open transaction and slot state needs no flip-flops.
- Up to 8 logical devices (e.g. a hub, a controller hub, four XIDs and a memory unit = 7); 32 slots (16 IN, 16 OUT).
- SOF frame number; suspend after about 5 ms of idle J.
- Testbench `sim/tb/tb_usb.v` (`make sim-usb`): a host model with its bit clock off by ±0.25 % drives control transfers, a deferred SET_ADDRESS, a second device at address 0, interrupt and bulk endpoints, NAK, STALL, toggles, retries, bad CRC, SOF and bus reset.

### 9.3 MSC bulk data

The SD card is on the ESP32 (00-system D8). MSC bulk data is ordinary slot traffic: the ESP32 writes 64 B packets into IN slots / reads OUT slots over the link. At 20 MHz the link (~2.5 MB/s) is well above the ~1 MB/s USB FS bulk ceiling.

## 10. Resource estimate

| Block | LUT4 (est.) | EBR | SPRAM |
|---|---|---|---|
| LPC target (mem r/w, I/O r/w) + I/O bus | 500 | 0 | |
| 16550 + Super I/O + FIFOs + UART PHY | 450 | 2 | |
| Mailbox | 120 | 2 | |
| iotrap (8 windows, shadow, FIFO writer, bankmap) | 400 | 1 | 1 |
| NOR ctrl + arbiter + prefetch + suspend | 500 | 2 | |
| flash_emu | 250 | 0 | |
| CRC | 120 | 0 | |
| Link + regfile + CDC + boot_seq | 550 | 1 | |
| USB PHY + SIE + 32 slots (measured) | 850 (≈ 1.3 k LCs) | 8 | |
| LED + misc | 100 | 0 | |
| **Total** | **≈ 4.5 k (85 %)** | **12** | **1** |

Still above a comfortable fill (~80 %) for nextpnr at 48 MHz.

**Measured (UPduino builds):** `lpc_com` (LPC target, COM + Super I/O, boot control, banks, mailbox, NOR engine, link) is 4181 LCs (79 %); the NOR engine alone is ~970 LUTs. Adding the USB core makes 5514 LCs (104 %), so the UP5K can't hold both. `lpc_usb` drops the mailbox and the NOR engine for USB bring-up: 3669 LCs (69 %), all clocks met (48 MHz domain 52 MHz).

**Measured on the UPduino (lpc_probe, ~1.6 k LCs):** routing, not logic, dominates paths at 48 MHz on the UP5K — typical failing paths were 6–8 ns logic + 14–16 ns routing. Wide counter compares, 16:1 register muxes feeding an output, and multi-condition enables each failed 48 MHz until pipelined. Rules for every `clk_sys` module: flag counter terminal counts a cycle early; register mux outputs; decode events into registered one-hot flags; count up from zero; no wide magnitude compares. Budget the full design for ≤ ~75 % fill. Mitigations, in order: merge mailbox into iotrap deferred mode (−100), drop Super I/O as a build option. No larger UP5K-pin-compatible part exists; if the mitigations aren't enough, see §10.1.

### 10.1 Fallback FPGA options [OPEN]

What a replacement must provide: **more LUTs** (≥ ~7 k for margin), ≥ ~16 KB block RAM, a PLL for 48 MHz, 3.3 V I/O, a way to self-boot fast, and the ability to read ROMs from an external NOR after configuration. It does **not** need in-package RAM — the ROMs stay in the NOR (D1), so a PSRAM die is cost we wouldn't use.

| Part | LUT4 | Block RAM | Config | Toolchain | Fit |
|---|---|---|---|---|---|
| iCE40UP5K (baseline) | 5280 | 15 KB EBR + 128 KB SPRAM | External SPI flash | Yosys/nextpnr-ice40 | Current target |
| iCE40HX4K-TQ144 | 3520 nominal; the die is the HX8K's and the open toolchain can use 7680 | 10 KB EBR nominal, no SPRAM | External SPI flash | Same as now | Cheapest move in LUTs-per-effort: same tools, same USB PHY approach. Loses SPRAM (iotrap FIFO shrinks to ~1 K entries in EBR), RGB driver and internal osc; higher static power; TQFP-144 |
| GW1N-4 / GW1NR-4 | 4608 | 22.5 KB BSRAM (+ PSRAM die on NR) | Internal flash (instant-on) | Gowin EDA or Apicula | **Fewer LUTs than the UP5K** — a sidegrade; only worth it for instant-on |
| **GW1N-9** (no PSRAM) | 8640 | 58.5 KB BSRAM | Internal flash, or MSPI from external flash | Gowin EDA or Apicula/nextpnr-himbaechel | Best Gowin fit: the LUTs we'd want without paying for the PSRAM die. ROMs stay in an external NOR on user pins |
| GW1NR-9 (Tang Nano 9K) | 8640 | 58.5 KB + 8 MB PSRAM | as GW1N-9 | as GW1N-9 | Overkill for us — the PSRAM goes unused with D1 |

Gowin costs to accept: toolchain change (open flow is usable but younger than icestorm), the USB PHY needs re-validating on new fabric (Gowin's own SoftPHY is vendor-licensed), and ESP32 bitstream updates go over JTAG (internal flash) unless booting from external flash in MSPI mode. Prices vary a lot by distributor (single-unit Mouser listings put GW1N-4 and GW1N-9 at roughly US$20 and US$33; LCSC is usually far lower) — check LCSC before choosing.

## 11. Repository layout & flow

```
fpga/
  rtl/{common,lpc,mem,probe,uart}/*.v        (later: boot, link, usb, led)
  boards/<board>/top_<design>.v, <board>.pcf, <design>_clocks.py      upduino31 (first) | openox_v1
  sim/models/   spi_flash_model.v
  sim/tb/       tb_<design>.v (Icarus Verilog)
  tools/        UART report viewers, bin2hex
  Makefile      DESIGN=<name>: yosys → nextpnr-ice40 --up5k --package sg48 → icepack; make sim; make prog; make rom ROM=<file>
```

## 12. Verification plan

| Test | Proves |
|---|---|
| Cold boot: boot_seq parses valid/invalid/two-record configs, time from config-done to LPC-ready | F-BOOT-1 |
| Mem read all sizes, mirrors, non-0xFF addresses ignored | Decode |
| Host abort, back-to-back cycles, LCLK stop/start, SYNC timeout + orphan drop | Robustness |
| LPC read during NOR erase → suspend/serve/resume within timeout | §4.7 |
| `ARM_AT_RESET` with 1-LCLK reset pulses; bank-select fast path | Bank switching |
| LFRAME# abort window and boot hold (§4.4) | 1.6 |
| 16550 polled TX with back-pressure, RX, reset drain; RTS/CTS | COM |
| Super I/O enable sequence | COM discovery |
| iotrap: capture order under burst, shadow reads, deferred read + timeout, FIFO full back-pressure | Legacy |
| flash_emu: autoselect ID, program, sector/chip erase, DQ7/DQ6 polling, protected slot | Flasher compatibility |
| Link: all spaces, NOINC, dummy/underrun, 20 MHz and slow SCK | Contract |
| USB: SETUP/IN/OUT/ZLP, NAK, STALL, toggles, bus reset, two devices at different addresses + one at 0 | SIE |
| NOR reads at `NOR_WIDTH` 1/2/4, small- and 16 MB layouts | Boards |
