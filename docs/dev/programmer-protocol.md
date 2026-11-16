# Programmer protocol

How a host talks to the device's ESP32 to manage ROMs, boot them and update the device. The console reaches it through four LPC I/O ports, and the PC through `oxctl`. The packets are the same on any byte stream. The definitions and a client are in [`libs/oxprog`](#libsoxprog), shared by the ESP32 firmware, OpenOX and any other project.

## Console port

Four LPC I/O ports at `0x0B00`, answered by the FPGA's mailbox: a byte FIFO each way, emptied and filled by the ESP32 over the link (see [Link](link.md#mailbox)).

| Port | Read | Write |
|---|---|---|
| +0 Data | Next byte of a reply | Next byte of a command |
| +1 Available | Bytes Data has to read, at most `0x7F` | |
| +2 Space | Bytes Data takes, at most `0x7F` | |
| +3 Id | `'O'` ready, `'R'` resetting or programmer not up | `0x01`: drop both FIFOs and end any transfer |

Counts never have bit 7 set, so `0xFF` (nothing on the bus) is never taken for one. A host checks `Id` once, writes `0x01` to start clean, waits for `'O'`, then writes a packet while `Space` allows and reads the reply while `Available` has bytes.

`Id` reads `'R'` until the ESP32 has set `MBX_CTRL.READY`, and again from a `0x01` write until the ESP32 has drained the reset. A host that only finds `'R'` for longer than a second treats the programmer as absent.

## Packets

Every command and reply is a 16-byte header followed by `Length` bytes of payload. Multi-byte fields are little-endian, structures packed.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `'O','X','F','P'` |
| 4 | 1 | Command: bits 3:0 mode, bits 7:4 action |
| 5 | 1 | Status (0 in commands) |
| 6 | 2 | Payload length, at most 4096 |
| 8 | 4 | CRC-32 of the payload (0 if none) |
| 12 | 4 | CRC-32 of bytes 0–11 |

CRC-32 is IEEE (zlib). The device answers every command with exactly one reply carrying the same command byte. A host skips bytes until it sees the magic, and after a bad reply resets the port before the next command. Replies may be longer or shorter than the host's structure: a newer device adds fields at the end, an older one sends fewer, and the missing ones read as 0.

## Commands

| Mode | Action | Payload → reply | What it does |
|---|---|---|---|
| Control (1) | 0 GetInfo | (none) → `Info` | Versions, board, boot flags, catalogue summary |
| | 1 Reboot | | Restarts the ESP32 after replying |
| | 2 SmbusRead | `SmbusTransfer` → `SmbusTransfer` | One byte read on the console's SMBus |
| | 3 SmbusWrite | `SmbusTransfer` | One byte write |
| | 4 Boot | `BootRequest` | Arms an entry for one boot ([Boot flow](boot-flow.md#one-shot-armed-boots)); with `BootReset` also resets the console through the SMC |
| | 5 ConfirmBoot | | Confirms the boot running now (a trial System, see [Boot flow](boot-flow.md#trial-updates)) |
| Upload (2) | 0 Begin | `UploadBegin` | Type, size, name; picks the slot |
| | 1 Write | `UploadWrite` | Offset + up to 4092 bytes |
| | 2 Commit | `UploadCommit` → `RomEntry` | Checks the CRC, programs and verifies; ROM types reply the new entry |
| | 3 Abort | | Drops the transfer |
| Download (3) | 0 Begin | `DownloadBegin` → `DownloadInfo` | Size and CRC |
| | 1 Read | `DownloadRead` → data | Up to 4096 bytes |
| | 2 End | | |
| Rom (4) | 0 List | `RomList` → `RomEntry[]` | Entries from `First`; a reply with fewer than 77 is the last |
| | 1 Get | `RomRef` → `RomEntry` | |
| | 2 Delete | `RomRef` | User ROMs and spare slots only |
| | 3 Rename | `RomRename` | |
| | 4 SetUser | `RomRef` | The power-button ROM, `0xFF` for none |
| | 5 Compact | `RomCompact` → `RomCompactStep` | Moves one entry towards a free run of `ImageSize`; repeat until `Moved` is `0xFF` |

Upload types: retail tables, user ROM, Loader, System, Diag, ESP32 firmware, FPGA bitstream. The last two update the device itself with the console running ([ESP32](esp32.md#updates)): the bitstream's Commit writes and reloads the FPGA (a few seconds), the firmware's Commit replies and then restarts the ESP32; neither replies with an entry. Download types: retail tables, a catalogue entry, the ROM being served.

Commit flags: `Trial` (Loader, System: boot once on trial, current only after confirming); `User` (make a user ROM the power-button ROM); `Boot` (boot it from the console's next reset). An upload from the console of a Loader or System is always on trial.

The console can't erase or program the slot it is running from, or a current Loader, System or Diag (`Protected`). Uploads go to a free slot of the right size or the role's spare slot: `Fragmented` if `Compact` can free one, `NoSpace` if not.

## Status

| Value | Name | |
|---|---|---|
| 0 | Ok | |
| 1 | ConsoleOn | Needs the console off |
| 2 | ProgrammerBusy | Another transfer is running |
| 3 | LayoutInvalid | Flash layout or catalogue unreadable |
| 4 | NotStarted | Write/Commit/Read without Begin |
| 5 | BadSize | |
| 6 | BadOffset | |
| 7 | BadCommand | Unknown mode or action |
| 8 | FlashError | Program or verify failed |
| 9 | CrcMismatch | |
| 10 | NotFound | No such entry, or nothing stored |
| 11 | Protected | |
| 12 | NoSpace | |
| 13 | ConsoleOff | Needs the console on |
| 14 | SmbusError | Nothing acked |
| 15 | Fragmented | No free run of that size, but `Compact` can make one |

`0xFE` Timeout and `0xFF` TransportError are reported by the client only.

## `libs/oxprog`

The definitions above, and a client for C++ and C, shared by the ESP32 firmware, OpenOX and any other host. How to use it is in [Building](building.md#libsoxprog).
