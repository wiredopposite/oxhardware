# 06 — ROMs and Boot Flow

How the chip decides what the Xbox boots: the ROM roles, the Loader's decision, one-shot armed boots, failed-boot detection, trial updates and forced boots. Registers are in 01-interface §5.2, the boot config record in §6.2.

## 1. ROM roles

| Role | What it is | Slot size | Slots | Written by |
|---|---|---|---|---|
| **Loader** | Boot default. Runs on every power-up and reset with nothing armed, reads the boot reason and boots the right bank | 256 K | 2 (A/B) | Update packages only |
| **System** | The OpenOX kernel with the setup UI (ROM library, settings, updates for the ESP32 and FPGA) | up to 1 M | 2 (A/B) | Update packages only |
| **Diag** | RAM and hardware test, booted on command | 256 K | 1 | Update packages only |
| **User** | Users' own ROMs | 256 K / 512 K / 1 M | the rest | `oxctl`, the System UI |

Loader, System and Diag slots are in `PROTECT_MASK`: console-side flashing can't touch them, and `rom_mgr` refuses to write them outside an update. Which slots hold which role is recorded in the catalogue (03-esp32 §5); the FPGA only needs the banks, which the boot config record supplies (01-interface §6.2).

## 2. Switching banks means a reset

The CPU can't jump from one BIOS into another. At reset the MCPX runs the X-codes from the start of the flash window to initialise the hardware, then the image's own loader; a BIOS image only works when started that way. So every switch is:

1. Arm the target bank for the next reset.
2. Reset the console through the SMC (SMBus 0x10, register 0x02 = 0x01).

The Loader, System and Diag run on the Xbox CPU and reach the SMC through the MCPX's SMBus controller. The ESP32 reaches it through its own SMBus master (03-esp32 §13).

## 3. One-shot armed boots (FPGA)

| Register | Meaning |
|---|---|
| `DEFAULT_BANK` | Served after any reset with nothing armed. Set from the boot config record: the confirmed Loader. |
| `PENDING_BANK` + `ARM_AT_RESET` | Served for **one** boot: at the next `LRESET#` assertion it becomes `ACTIVE_BANK` and the arm clears. |
| `ACTIVE_BANK` | What is being served now. |

At every `LRESET#` assertion: if armed, `ACTIVE_BANK ← PENDING_BANK`, `ARMED_BOOT` is set and `TRIAL ← TRIAL_PENDING`; otherwise `ACTIVE_BANK ← DEFAULT_BANK` and both clear. The arm, `TRIAL_PENDING` and `CONFIRMED` clear either way. A bank that crashes, FRAGs or reboots therefore never runs twice by accident: the reset after it lands on the Loader.

While `LRESET#` is held low (console off, or in reset) an arm takes effect at once, the same way. Power-on brings no falling edge, so this is what makes an arm made with the console off apply to its next boot.

The console arms through the boot control port (§4); the ESP32 through the link registers. Both set the same `PENDING_BANK`.

## 4. Boot control port (console side)

What the Loader uses: eight LPC I/O ports at `BOOTCTL_BASE` (default `0x00E0`, from the boot config record), answered by the FPGA itself and always claimed while `SERVE_EN` is set. Bank values use the `ACTIVE_BANK` format.

| Port | Read | Write |
|---|---|---|
| +0 | `0x4F` signature | — |
| +1 | `BOOT_FLAGS` (below) | Bit 3 set: clear `LAST_FAILED` |
| +2 | Armed target (`0xFF` = none) | Arm this bank for the next reset (`0xFF` disarms) |
| +3 | — | `0xA5`: confirm this boot (a trial Loader, §7) |
| +4 | `USER_BANK`: bank for power-button boots | — |
| +5 | `SYSTEM_BANK`: confirmed System | — |
| +6 | `DIAG_BANK` | — |
| +7 | `ACTIVE_BANK` | — |

`BOOT_FLAGS`: bit0 a bank is armed; bit1 `ARMED_BOOT` (this boot was armed); bit2 `TRIAL` (this boot must confirm); bit3 `LAST_FAILED` (the previous armed boot failed, set by the ESP32); bit4 `CONFIRMED` (this boot has confirmed); bit5 `TRIAL_PENDING` (the armed boot will be a trial); bit6 `TRIAL_CONFIRMED` (a trial boot confirmed with `0xA5`; stays set across resets until the ESP32 clears it).

Every value the Loader needs comes from the boot config record, so the Loader works before the ESP32 has started, or without one. System and other console software use the programmer protocol (07-console-protocol) instead, which goes through the ESP32: ROM library, choosing the power-button ROM, booting an entry, confirming, updates.

## 5. Normal boot

1. Power-up or reset with nothing armed: the FPGA serves the Loader (`DEFAULT_BANK`).
2. The Loader reads the boot reason from the SMC and `BOOT_FLAGS`:

   | Condition | Target |
   |---|---|
   | `LAST_FAILED` set | System; it shows which bank failed and clears the flag |
   | Eject | System |
   | Power button | `USER_BANK`; System if none is set |

3. The Loader writes the target to port +2 and resets the console through the SMC.
4. The target boots (one-shot). Its next reset lands on the Loader again.

Eject therefore always reaches System, whatever is in the user banks.

## 6. Failed-boot detection (ESP32)

User ROMs are stock code and can't report anything, so the ESP32 judges every armed boot from what the FPGA sees: `LRESET#` edges and `MEM_READ_COUNT` against time.

Measured on a 1.6 console with a 256 K image:

| | Good boot | Failed boot (bad image, or nothing served) |
|---|---|---|
| Memory reads after `LRESET#` deasserts | ~267 K, all within 0.5 s | Stops at 1.4–2.5 K |
| `LRESET#` | Stays high | The SMC retries: bursts of 4 resets ~0.37 s apart, three bursts, then FRAG with no further resets |
| SMC over SMBus during the FRAG | — | Still answers reads |

**Rule:** an armed boot has failed if `LRESET#` asserts again within 3 s of deasserting, or fewer than 64 K reads have been served 2 s after it. On failure the ESP32:

1. sets `LAST_FAILED` and records the bank in the catalogue (`FAILED_BOOT`);
2. if the console is not already resetting by itself, resets it through the SMC (reset, then power cycle if the SMC doesn't respond).

The one-shot arm has expired by then, so the console comes back to the Loader, which sends it to System (§5).

## 7. Trial updates (Loader, System)

An update never overwrites the running image:

1. `rom_mgr` writes the new image into the role's spare slot (A/B) and verifies it with the FPGA CRC engine.
2. The ESP32 arms the new slot with `TRIAL_PENDING` set. The trial starts at the next reset, or at once if the console is off or in reset (§3); the console can also be reset straight after the upload.
3. The new image confirms once it is up. System: `ConfirmBoot` (07-console-protocol §3) after its UI has started. Loader: `0xA5` to port +3 before it resets into its target, which sets `TRIAL_CONFIRMED`.
4. Confirmed → the ESP32 makes it current: for the Loader, a new boot config record with `DEFAULT_BANK` = the new slot; for System, the catalogue and `SYSTEM_BANK`. The old slot becomes the spare.
5. Not confirmed within 10 s of `LRESET#` rising, or a reset before it confirms → the entry is marked failed-boot and the ESP32 sets `LAST_FAILED`. On a timeout the ESP32 also resets the console; the next boot lands on the old image.

Only one trial runs at a time; another trial upload, or deleting the entry on trial, is refused with `ProgrammerBusy`. An arm for anything else cancels a trial that hasn't started. The trial state is kept in the catalogue, so an ESP32 restart picks it up again if the FPGA still shows it.

`DEFAULT_BANK` only ever points at a confirmed Loader, and the Loader only sends eject to a confirmed System, so neither can be replaced by an untested image.

## 8. Forced boots (ESP32)

`oxctl rom boot <bank>` and `oxctl rom diag` (or `Boot` from the console, 07-console-protocol §3) arm a bank from the ESP32 and reset the console through the SMC, skipping the Loader's decision. The arm is one-shot as above, and failure detection (§6) applies. This also covers recovery when the Loader slot itself is unusable: the ESP32 can force-boot System, and can rewrite the Loader slot over the config bus.
