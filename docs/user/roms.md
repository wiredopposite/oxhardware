# ROMs

The device keeps a library of BIOS images in the flash behind the FPGA. On the UPduino's 4MB flash that's fifteen 256KB slots. A ROM takes one, two or four slots, for a 256KB, 512KB or 1MB image.

## Roles

Each entry has a role:

| Role | What it is |
|---|---|
| Loader | Runs on every power-up and reset, and boots the right ROM. 256KB, with a spare slot for updates |
| System | OpenOX with its boot menu, which manages the library, settings and updates. Up to 1MB, with a spare slot |
| Diag | A RAM and hardware test, booted on command. 256KB |
| User | Your own ROMs |

The Loader, System and Diag only change through updates. Console software can't overwrite them, or the ROM that's running.

## What boots

| Press | Boots |
|---|---|
| Power | The power-button ROM, one of your user ROMs. System if there's none |
| Eject | System |

Eject always reaches System, whatever is in the user slots. If a ROM fails to boot, the next boot goes to System, once. `rom list` marks the one that failed `failed-boot`.

A ROM can also be booted once on its own, from the boot menu or `oxctl rom boot`. The boot after that goes back to the Loader.

## Managing the library

From the PC, with [oxctl](oxctl.md):

```
python tools/oxctl.py -p COMx xbox off
python tools/oxctl.py -p COMx rom upload mybios.bin --name mybios --user
python tools/oxctl.py -p COMx rom list
```

Turn the console off before uploading from the PC. The flash is shared with the running console, and an upload while it's on fails its CRC check.

From the console, OpenOX's boot menu has a Banks page for the same: upload a ROM from a drive, rename, set the power-button ROM, boot one now, delete. Those uploads are made while the console runs from RAM, so they work with it on.

An image that repeats its first 256KB or 512KB is stored as that, since the console sees the same bytes either way.

If there's room in total but no free run of slots the right size, the upload fails with `Fragmented`, and `rom compact` (or `--compact` on the upload) moves entries to make one.

## Updates

Loader and System updates boot once on trial. They only replace the old ones after they've started and confirmed. If one doesn't, or the console is turned off before it does, the next boot lands on the old one. Each role keeps its current ROM and one spare: a new update replaces the spare, and the one it replaces becomes the spare once it's confirmed. See [Boot flow](../dev/boot-flow.md#trial-updates).

The FPGA's bitstream and the ESP32's firmware are updated with `oxctl update`, with the console running.
