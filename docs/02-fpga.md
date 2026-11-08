# 02 — FPGA Gateware Specification (iCE40UP5K)

## 1. Device budget

| Resource | UP5K | Planned use |
|---|---|---|
| LUT4 / FF | 5280 | ~4.0–4.6 k estimated (§11) — tight; features are build-time parameters |
| EBR (512 B each) | 30 | USB slot buffers (4), COM FIFOs (2), mailbox (2), NOR prefetch + program buffer (2), shadow RAM + link FIFOs (2) |
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
| `clk_sck` | `LINK_SCK` | ≤ 40 MHz | Link shift registers |

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
    ├── usb/{usb_phy_rx,usb_phy_tx,usb_sie,usb_slots}.v
    └── led/status_led.v
```

Build-time parameters (`FEATURE_*`) let a smaller bitstream drop iotrap/flash-emu/Super I/O if LUTs run out. `NOR_WIDTH` (1/2/4) selects single, dual-output (`0x3B`) or quad (`0xEB`) reads per board.

Vendor primitives (`SB_IO`, `SB_PLL40_PAD`, `SB_RGBA_DRV`, `SB_SPRAM256KA`) appear only in `boards/` and thin wrappers in `rtl/common/prim_*.v`, so a fallback FPGA (§11.1) means new wrappers, not a rewrite.

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
- **Memory write**: claim if `FE_CTRL.enable && A[31:24] == 0xFF`; pass `{A[19:0], D}` to `flash_emu`. Not enabled → don't claim (stay off the bus). [VERIFY: does the MCPX/Xbox ever issue memory writes to 0xFF… at boot that we must *not* answer?]
- **I/O**: exact 16-bit decode per device; unclaimed cycles are not answered.
- **SYNC**: short wait `0101` until ready, then `0000`. `SYNC_TIMEOUT` → `1010` once, TAR, release, count, `ERROR`. Deferred iotrap reads and back-pressured writes use the same timeout.
- **TAR**: drive `1111` on the first TAR clock after our SYNC/data, then tri-state.
- **Reset**: `LRESET#` low → FSM idle, LAD/LFRAME# released (D0 is not, §4.5), 16550/Super I/O/mailbox state reset. iotrap shadow RAM is *not* reset (ESP32-owned state).

### 4.3 Address → NOR

`nor_addr = ROM_BASE + {ACTIVE_BANK.SLOT, 18'b0} + (A[19:0] & size_mask)`. Mirrors 256/512 K images through the 1 MB window and the window through all of `0xFF000000–0xFFFFFFFF`.

### 4.4 1.6 — LFRAME# abort

With `LFRAME_ABORT`, once a memory read is claimed (known after A27..24 at clock 3), drive `LFRAME#` low (open-drain) from clock 4/5 through our final TAR clock, releasing so the last clock isn't a START (LAD = 1111). Refs: ox `responder.pio`, EOS `serving_mem`. Needs the physical LPC rebuild.

### 4.5 D0 (1.0–1.5)

`D0_N` open-drain low whenever `SERVE_EN && D0_EN`, **including during `LRESET#`**; otherwise Hi-Z → TSOP. Unconfigured FPGA = Hi-Z, so a failed/slow boot falls back to TSOP. With self-boot (~20–70 ms) D0 should be asserted before the MCPX leaves reset [VERIFY by measurement, 00-system §4.4].

### 4.6 Read latency & prefetch

NOR quad I/O fast read `0xEB` in continuous-read mode (no command byte after the first): 6 address + 2 mode + 4 dummy + 2 data = 14 clocks at 48 MHz ≈ 290 ns + CDC ≈ 13–15 LCLK per uncached byte (~12 waits). `rom_prefetch` fetches the aligned 32-byte line on a miss (~1 µs) and serves hits with zero waits; the MCPX reads sequentially, so most reads hit. Invalidate on bank change, flash-emu write, or link program/erase. SPRAM cache or a faster NOR clock (72–96 MHz) are later options.

### 4.7 NOR busy vs. LPC reads

A single NOR can't be read during erase/program. If an LPC read arrives while busy, `nor_qspi` issues **suspend** (`0x75`, tSUS ≤ ~20–30 µs [VERIFY]), serves the read (SYNC waits; well under the 61 µs timeout), and **resumes** (`0x7A`) after the bus has been idle ~10 µs. Flash-emulation operations are exempt — their status reads are answered by `flash_emu` per JEDEC (§6).

## 5. COM port

### 5.1 16550 model — ox semantics (`refs/ox/src/lpc/uart.cpp`)

- Registers at `COM_BASE+0..7`: RBR/THR, IER, IIR/FCR, LCR, MCR, LSR, MSR, SCR; DLL/DLM with DLAB. Reset values as ox (`DLL=1, LCR=0x03`).
- **Guest baud is ignored**: DLL/DLM stored and read back only (also exposed as `COM_GUEST_DIV`). Physical baud comes from `UART_DIV`.
- **Back-pressure**: LSR.THRE|TEMT (0x60) only while the TX FIFO has room — a polled sender waits instead of dropping. LSR.DR = RX non-empty.
- IIR = 0x01 (no interrupt) | 0xC0 if FCR enabled FIFOs. IER stored (masked 0x0F). MSR = 0xB0 (DCD|DSR|CTS). MCR stored (0x1F); MCR.LOOP loops THR→RBR. FCR bit1 clears RX. SERIRQ not implemented.
- On `LRESET#`: registers to defaults, RX FIFO drained, stale TX dropped (ox drains `fromConsole` on reset).

### 5.2 Physical side

`uart_phy` 8N1 at `48 MHz / UART_DIV` to bridge channel B. With `RTSCTS`: deassert RTS when RX FIFO > 3/4 full; stop TX while CTS deasserted. FIFOs 512 B each. `ROUTE=1` connects the FIFOs to link SPACE 3 port 0 instead.

### 5.3 Super I/O

SMSC LPC47M-style (`refs/xprobe/pico/src/lpc/superio.cpp`): 0x55 enter / 0xAA exit at 0x2E (or 0x4E); 0x07 LDN; per-LDN 0x30 activate, 0x60/0x61 base. LDN 4 activation overrides `COM_BASE`. Device ID reg 0x20 value for the debug-kit part [VERIFY].

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

| Chip | Typical I/O (to confirm from OpenXenium / XBMC4Xbox LCD drivers) [VERIFY all] | iotrap use |
|---|---|---|
| Xenium / OpenXenium | `0xEF` bank select (+ status read), `0xEE` LED/LCD control bits | `0xEE` capture + shadow; `0xEF` bank-select fast path + shadow |
| SmartXX | LCD data/command ports in the `0xF7xx` range, LED port | capture + shadow |
| Xecuter 3 | LCD ports in the `0xF5xx`/`0xF7xx` range | capture + shadow |
| Aladdin / others | LCD ports | capture + shadow |

### 6.3 JEDEC flash emulation (`flash_emu`)

Lets existing Xbox flashers (which talk to a parallel/LPC flash chip with AMD/SST-style command sequences through memory cycles at `0xFFxxxxxx`) write ROMs into the active bank.

- Address decode uses `A[19:0] & size_mask` (unlock addresses 0x5555/0x2AAA or 0x555/0x2AA per the emulated part [VERIFY]).
- Commands: reset (`F0`), autoselect (`90` → reads return `FE_ID` mfr/device at offsets 0/1), byte program (`A0`), sector erase (`80`,`30`), chip erase (`80`,`10` → **active bank only**), status polling: DQ7 data-polling and DQ6 toggle while busy.
- Byte program → buffered into the NOR program buffer; flushed as a page program when the next byte leaves the page or after ~50 µs idle (status polling reports "busy" until flushed).
- Sector size reported to the flasher should match a NOR erase size (4 KB, like SST49LF0x0) [VERIFY flashers' assumptions].
- `PROTECT_MASK` slot → command ignored, `FLASHEMU_PROTECT` error, status returns "done" (flashers then fail verify, safely).
- Raises `FLASHEMU` on sequence start/finish so the ESP32 can update the catalogue (CRC, "modified by console").

## 7. NOR controller

Target part: W25Q128JV-class (16 MB, 3.3 V, quad, 4 KB sectors, erase/program suspend) [VERIFY].

- Boot: release power-down `0xAB`, set QE (status reg 2) once at factory, quad I/O read `0xEB` with continuous mode.
- Ops: 4 KB erase `0x20`, 64 KB erase `0xD8`, page program `0x02` (or quad `0x32`), status `0x05/0x35`, suspend `0x75`, resume `0x7A`.
- Region guard: anything below `ROM_BASE` refused unless `NOR_UNLOCK`.
- SCLK = 48 MHz via DDR output register; capture on a delayed/opposite edge — validate margins in nextpnr and on hardware.
- **UPduino v3.1**: 4 MB flash on the config pins; the board is described as "qSPI capable" but whether IO2/IO3 reach FPGA pins is [VERIFY on schematic]. If not, build with `NOR_WIDTH=2` (dual-output read `0x3B` uses only SO/SI). A random byte then costs ~44 SCLKs (~0.9 µs at 48 MHz, ~1.8 µs at 24 MHz) vs ~20 in quad continuous mode — fine for bring-up because MCPX fetches are mostly sequential and hit the prefetch line, but it is a good stress test of the long-wait SYNC path. Small-flash layout per 01-interface §6.1.

## 8. ESP32 link

Implements 01-interface §3. MISO tri-stated while `LINK_CS_N` high. `IRQ_N` push-pull active-low. Link is not reset by `SOFT_RESET`.

## 9. USB full-speed device

### 9.1 Electrical

`USB_DP`, `USB_DN` (bidirectional `SB_IO`), `USB_PU` (1.5 kΩ to D+; Hi-Z = detached), ~27 Ω series on D+/D− (as pico_usb's PIO requirement). Connects to the MCPX controller-port test pads.

### 9.2 Architecture

```
 D+/D- ─► 4× oversample DPLL ─► NRZI dec ─► unstuff ─► packet rx (PID, CRC5/16)
                                                          │
                usb_sie: token → device match (addr table, "answers at 0") → slot match (dev, ep, dir) → handshake
                                                          │
 D+/D- ◄─ NRZI enc ◄─ stuff ◄─ packet tx (SYNC, PID, data, CRC16, EOP)
                                         ▲
              usb_slots: 32 × 64 B EBR + per-slot {enable, stall, dir, type, dev, ep, toggle, armed, len}
```

- Model: valentyusb rx/tx (BSD-3, proven on UP5K at 48 MHz) ported to Verilog; SIE behaviour mirrors pico_usb's PIO DCD (`dcd_pio.cpp`: per-device address match, unaddressed tokens ignored, SET_ADDRESS applied after the status ZLP).
- Up to 4 logical devices (composite = 1; hub emulation = hub + up to 3 downstream, e.g. XID + XID + MSC, or Duke-like hub + XID + MU).
- Turnaround: response within 2–7.5 bit times; handshake wait 16–18 bit times. ESP32 never in that loop.
- Slot lookup: 32-entry parallel compare of `{dev, ep, dir}` (≈ 32 × 7-bit comparators) — register-file implementation, one cycle.
- SOF frame number; suspend after 3 ms idle J.

### 9.3 MSC bulk data

The SD card is on the ESP32 (00-system D8). MSC bulk data is ordinary slot traffic: the ESP32 writes 64 B packets into IN slots / reads OUT slots over the link. At 40 MHz link clock that is well above the ~1 MB/s USB FS bulk ceiling.

## 10. (removed — SD block engine moved to the ESP32)

## 11. Resource estimate (replace with Yosys numbers per phase)

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
| USB PHY + SIE + 32 slots | 1500 | 4 | |
| LED + misc | 100 | 0 | |
| **Total** | **≈ 4.5 k (85 %)** | **12** | **1** |

Still above a comfortable fill (~80 %) for nextpnr at 48 MHz.

**Measured on the UPduino (lpc_probe, ~1.6 k LCs):** routing, not logic, dominates paths at 48 MHz on the UP5K — typical failing paths were 6–8 ns logic + 14–16 ns routing. Wide counter compares, 16:1 register muxes feeding an output, and multi-condition enables each failed 48 MHz until pipelined. Rules for every `clk_sys` module: flag counter terminal counts a cycle early; register mux outputs; decode events into registered one-hot flags; count up from zero; no wide magnitude compares. Budget the full design for ≤ ~75 % fill. Mitigations, in order: reduce USB slots to 16 (−300), merge mailbox into iotrap deferred mode (−100), drop Super I/O as a build option. Get real numbers early (phase 1 build with stubs on the UPduino). A UP5K-pin-compatible larger part does not exist; if the mitigations aren't enough, see §11.1.

### 11.1 Fallback FPGA options [OPEN — decide on real Yosys numbers]

What a replacement must provide: **more LUTs** (≥ ~7 k for margin), ≥ ~16 KB block RAM, a PLL for 48 MHz, 3.3 V I/O, a way to self-boot fast, and the ability to read ROMs from an external NOR after configuration. It does **not** need in-package RAM — the ROMs stay in the NOR (D1), so a PSRAM die is cost we wouldn't use.

| Part | LUT4 | Block RAM | Config | Toolchain | Fit |
|---|---|---|---|---|---|
| iCE40UP5K (baseline) | 5280 | 15 KB EBR + 128 KB SPRAM | External SPI flash | Yosys/nextpnr-ice40 | Current target |
| iCE40HX4K-TQ144 | 7680 usable via icestorm (HX8K die) [VERIFY] | 16 KB EBR [VERIFY], no SPRAM | External SPI flash | Same as now | Cheapest move in LUTs-per-effort: same tools, valentyusb already runs on iCE40 LP/HX. Loses SPRAM (iotrap FIFO shrinks to ~1 K entries in EBR), RGB driver and internal osc; higher static power; TQFP-144 |
| GW1N-4 / GW1NR-4 (Stellar's part) | 4608 | 22.5 KB BSRAM (+ PSRAM die on NR) | Internal flash (instant-on) | Gowin EDA or Apicula | **Fewer LUTs than the UP5K** — a sidegrade; only worth it for instant-on |
| **GW1N-9** (no PSRAM) | 8640 | 58.5 KB BSRAM | Internal flash, or MSPI from external flash | Gowin EDA or Apicula/nextpnr-himbaechel | Best Gowin fit: the LUTs we'd want without paying for the PSRAM die. ROMs stay in an external NOR on user pins |
| GW1NR-9 (Tang Nano 9K) | 8640 | 58.5 KB + 8 MB PSRAM | as GW1N-9 | as GW1N-9 | Overkill for us — the PSRAM goes unused with D1 |

Gowin costs to accept: toolchain change (open flow is usable but younger than icestorm), USB PHY needs porting/validating (valentyusb is generic RTL but timing-proven on iCE40; Gowin's own SoftPHY is vendor-licensed), and ESP32 bitstream updates go over JTAG (internal flash) unless booting from external flash in MSPI mode. Prices vary a lot by distributor (single-unit Mouser listings put GW1N-4 and GW1N-9 at roughly US$20 and US$33; LCSC is usually far lower) — check LCSC before choosing.

## 12. Repository layout & flow

```
fpga/
  rtl/{common,boot,link,lpc,mem,uart,usb,led}/*.v
  boards/<board>/top_<board>.v, <board>.pcf        upduino31 (first) | openox_v1
  sim/models/   nor_w25q.v, lpc_host.py, usb_host.py, spi_master.py
  sim/tests/    test_lpc_{mem,io,write}.py, test_flash_emu.py, test_iotrap.py, test_com.py, test_link.py,
                test_nor.py, test_boot_cfg.py, test_usb_{basic,multidev}.py
  Makefile      yosys → nextpnr-ice40 --up5k --package sg48 → icepack; make sim; make prog (iceprog / via ESP32)
```

## 13. Verification plan

| Test | Proves |
|---|---|
| Cold boot: boot_seq parses valid/invalid/two-record configs, time from config-done to LPC-ready | F-BOOT-1 |
| Mem read all sizes, mirrors, non-0xFF addresses ignored | Decode |
| Host abort, back-to-back cycles, LCLK stop/start, SYNC timeout + orphan drop | Robustness |
| LPC read during NOR erase → suspend/serve/resume within timeout | §4.7 |
| `ARM_AT_RESET` with 1-LCLK reset pulses; bank-select fast path | Bank switching |
| LFRAME# abort waveform vs. ox/EOS | 1.6 |
| 16550 polled TX with back-pressure, RX, reset drain; RTS/CTS | COM |
| Super I/O enable sequence | COM discovery |
| iotrap: capture order under burst, shadow reads, deferred read + timeout, FIFO full back-pressure | Legacy |
| flash_emu: autoselect ID, program, sector/chip erase, DQ7/DQ6 polling, protected slot | Flasher compatibility |
| Link: all spaces, NOINC, dummy/underrun, 40 MHz | Contract |
| USB: SETUP/IN/OUT/ZLP, NAK, STALL, toggles, bus reset, two devices at different addresses + one at 0 | SIE |
| NOR reads at `NOR_WIDTH` 1/2/4, small- and 16 MB layouts | Boards |
