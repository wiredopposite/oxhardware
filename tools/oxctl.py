#!/usr/bin/env python3
"""OpenOX PC tool.

    py -3 tools/oxctl.py -p COM12 sys version
    py -3 tools/oxctl.py -p COM12 sys status
    py -3 tools/oxctl.py -p COM12 sys ping
    py -3 tools/oxctl.py -p COM12 sys reboot
    py -3 tools/oxctl.py -p COM12 log level debug [tag]
    py -3 tools/oxctl.py -p COM12 fpga status
    py -3 tools/oxctl.py -p COM12 fpga id                       # FPGA flash JEDEC ID
    py -3 tools/oxctl.py -p COM12 fpga probe                    # config-bus wiring check
    py -3 tools/oxctl.py -p COM12 fpga write FILE [--addr 0x0]  # program the FPGA's flash
    py -3 tools/oxctl.py -p COM12 fpga read OUT --addr A --len N
    py -3 tools/oxctl.py -p COM12 fpga release                  # let a held FPGA boot
    py -3 tools/oxctl.py -p COM12 smbus read ADDR REG [--word]
    py -3 tools/oxctl.py -p COM12 smbus write ADDR REG VALUE [--word]
    py -3 tools/oxctl.py -p COM12 xbox status|reset|cycle|off  # through the SMC
    py -3 tools/oxctl.py -p COM12 link info                     # FPGA identity over the link
    py -3 tools/oxctl.py -p COM12 link probe                    # link wiring check
    py -3 tools/oxctl.py -p COM12 link test [--count N] [--mhz F]         # scratch register stress
    py -3 tools/oxctl.py -p COM12 link read ADDR [--len N] [--space S]
    py -3 tools/oxctl.py -p COM12 link write ADDR HEXBYTES [--space S]
    py -3 tools/oxctl.py -p COM12 link irq                     # FPGA interrupts taken, by bit
    py -3 tools/oxctl.py -p COM12 nor info                      # flash through the running FPGA
    py -3 tools/oxctl.py -p COM12 nor read OUT --addr A --len N
    py -3 tools/oxctl.py -p COM12 nor write FILE --addr A       # erase, program, verify
    py -3 tools/oxctl.py -p COM12 nor erase --addr A --len N
    py -3 tools/oxctl.py -p COM12 nor crc --addr A --len N
    py -3 tools/oxctl.py -p COM12 rom list                      # ROM library (docs/03 §5)
    py -3 tools/oxctl.py -p COM12 rom upload FILE [--name N] [--type user|loader|system|diag] [--user] [--boot] [--trial] [--reset] [--compact]
    py -3 tools/oxctl.py -p COM12 rom compact SIZE                # e.g. 1M: move entries to free a run
    py -3 tools/oxctl.py -p COM12 rom download ID|active OUT
    py -3 tools/oxctl.py -p COM12 rom delete|user ID
    py -3 tools/oxctl.py -p COM12 rom rename ID NAME
    py -3 tools/oxctl.py -p COM12 rom boot ID [--reset]
    py -3 tools/oxctl.py -p COM12 prog info                     # programmer protocol (docs/07)
    py -3 tools/oxctl.py -p COM12 prog smbus-read ADDR REG
    py -3 tools/oxctl.py -p COM12 prog smbus-write ADDR REG VALUE
    py -3 tools/oxctl.py -p COM12 prog confirm                  # confirm the trial boot running now
    py -3 tools/oxctl.py -p COM12 usb start [pad|hub]|stop|status  # USB device (test gamepad)
    py -3 tools/oxctl.py -p COM12 usb plug|unplug        # hub profile: gamepad on port 1

fpga id/read/write hold the FPGA in reset (CRESET low) for the duration: the Xbox
loses its BIOS meanwhile, and iceprog must not be running.
    py -3 tools/oxctl.py -p COM12 monitor            # print logs and events until Ctrl+C
    py -3 tools/oxctl.py -p COM12 monitor --reset    # reset the ESP32 first (boot log)

The port can also come from the OXCTL_PORT environment variable.
"""
import argparse
import os
import struct
import sys
import time
import zlib

from oxlink import Link, LinkError, Reader, wait_forever

SYS_PING, SYS_VERSION, SYS_STATUS, SYS_REBOOT = 0x0000, 0x0001, 0x0002, 0x0003
LOG_LEVEL = 0x0100
FPGA_INFO, FPGA_HOLD, FPGA_RELEASE = 0x0200, 0x0201, 0x0202
FLASH_ID, FLASH_READ, FLASH_ERASE, FLASH_WRITE, FLASH_CRC = 0x0203, 0x0204, 0x0205, 0x0206, 0x0207
FLASH_PROBE = 0x0208
LINK_READ, LINK_WRITE, LINK_PROBE, LINK_CLOCK, LINK_IRQ = 0x0300, 0x0301, 0x0302, 0x0303, 0x0304
IRQ_BITS = ["LRST_ASSERT", "LRST_DEASSERT", "BANK_SWAPPED", "USB", "IOTRAP", "MBX_RX", "COM_RX",
            "ERROR", "bit8", "bit9", "FLASHEMU", "BOOTCTL"]
SMBUS_READ, SMBUS_WRITE, SMC_POWER = 0x0400, 0x0401, 0x0402
PROG_XFER = 0x0500
NOR_INFO, NOR_READ, NOR_ERASE, NOR_WRITE, NOR_CRC = 0x0600, 0x0601, 0x0602, 0x0603, 0x0604
USB_START, USB_STOP, USB_STATUS, USB_PLUG = 0x0700, 0x0701, 0x0702, 0x0703
USB_STATES = ["disabled", "default", "addressed", "configured"]
SMC = 0x10

LEVELS = ["none", "error", "warn", "info", "debug", "verbose"]
RESET_REASONS = ["unknown", "power-on", "external", "software", "panic", "int-wdt",
                 "task-wdt", "wdt", "deep-sleep", "brownout", "sdio", "usb", "jtag",
                 "efuse", "power-glitch", "cpu-lockup"]


def cmd_sys(link, a):
    if a.what == "ping":
        data = os.urandom(32)
        if link.request(SYS_PING, data) != data:
            sys.exit("ping: echo mismatch")
        print("ping ok")
    elif a.what == "version":
        r = Reader(link.request(SYS_VERSION))
        ver, idf, built, rev = r.str(), r.str(), r.str(), r.u16()
        print(f"firmware {ver}  (built {built})\nESP-IDF  {idf}\nchip rev {rev // 100}.{rev % 100}")
    elif a.what == "status":
        r = Reader(link.request(SYS_STATUS))
        up, free, low, reason = r.u64(), r.u32(), r.u32(), r.u8()
        print(f"uptime {up / 1e6:.1f} s  heap free {free} (min {low})  "
              f"last reset {RESET_REASONS[reason] if reason < len(RESET_REASONS) else reason}")
    elif a.what == "reboot":
        link.request(SYS_REBOOT)
        print("rebooting")


def release(link):
    r = Reader(link.request(FPGA_RELEASE, struct.pack("<I", 1000), timeout=3))
    us = struct.unpack("<i", struct.pack("<I", r.u32()))[0]
    print(f"FPGA released: CDONE high after {us / 1000:.1f} ms" if us >= 0
          else "FPGA released: CDONE did not go high within 1 s")


def cmd_fpga(link, a):
    if a.what == "status":
        r = Reader(link.request(FPGA_INFO))
        cdone, held = r.u8(), r.u8()
        print(f"CDONE {'high (configured)' if cdone else 'low'}  "
              f"{'held in reset by the ESP32' if held else 'not held'}")
        return
    if a.what == "release":
        release(link)
        return
    if a.what == "probe":
        r = link.request(FLASH_PROBE)
        idle, idb = r[0], r[1:4]
        names = ["SS", "SCK", "SI", "SO"]
        print("undriven levels with the FPGA in reset (expect all 1, pulled up):")
        print("  " + "  ".join(f"{n}={(idle >> i) & 1}" for i, n in enumerate(names)))
        print(f"bit-banged flash ID: {idb.hex(' ').upper()}  (expect EF 40 16)")
        return
    link.request(FPGA_HOLD)
    try:
        idb = link.request(FLASH_ID)
        print(f"flash ID: {idb.hex(' ').upper()}")
        if idb in (b"\x00\x00\x00", b"\xff\xff\xff"):
            raise LinkError("no flash answering: check SS/SCK/SI/SO wiring")
        if a.what == "read":
            out = bytearray()
            while len(out) < a.len:
                n = min(4096, a.len - len(out))
                out += link.request(FLASH_READ, struct.pack("<IH", a.addr + len(out), n))
            with open(a.file, "wb") as f:
                f.write(out)
            print(f"read {len(out)} bytes from 0x{a.addr:06X} to {a.file}")
        elif a.what == "write":
            data = open(a.file, "rb").read()
            if a.addr & 0xFFF:
                raise LinkError("--addr must be a multiple of 4 KB")
            t0 = time.time()
            span = (len(data) + 0xFFF) & ~0xFFF
            for off in range(0, span, 0x10000):
                n = min(0x10000, span - off)
                link.request(FLASH_ERASE, struct.pack("<II", a.addr + off, n), timeout=10)
                print(f"\rerased {off + n}/{span}", end="", flush=True)
            print()
            for off in range(0, len(data), 4096):
                link.request(FLASH_WRITE, struct.pack("<I", a.addr + off) + data[off:off + 4096],
                             timeout=5)
                print(f"\rwritten {min(off + 4096, len(data))}/{len(data)}", end="", flush=True)
            print()
            crc = Reader(link.request(FLASH_CRC, struct.pack("<II", a.addr, len(data)),
                                      timeout=10)).u32()
            want = zlib.crc32(data)
            if crc != want:
                raise LinkError(f"verify failed: flash CRC {crc:08X}, file {want:08X}")
            print(f"verified {len(data)} bytes at 0x{a.addr:06X} (CRC {crc:08X}) "
                  f"in {time.time() - t0:.1f} s")
    finally:
        release(link)


STS_BITS = ["NOR busy", "error", "mailbox", "iotrap", "USB", "serving", "LRESET# high", "IRQ"]


def sts_text(sts):
    return f"STS {sts:02X}" + "".join(f" [{n}]" for i, n in enumerate(STS_BITS) if sts >> i & 1)


def link_read(link, addr, n, space=0):
    r = link.request(LINK_READ, struct.pack("<BIH", space, addr, n))
    return r[0], r[1:]


def link_write(link, addr, data, space=0):
    return link.request(LINK_WRITE, struct.pack("<BI", space, addr) + data)[0]


def cmd_link(link, a):
    if a.mhz:
        link.request(LINK_CLOCK, struct.pack("<I", int(a.mhz * 1e6)))
    if a.what == "info":
        sts, r = link_read(link, 0, 0x10)
        if r[0:4] != b"OXFG":
            raise LinkError(f"no FPGA answering: ID {r[0:4].hex(' ').upper()} (expect 4F 58 46 47), {sts_text(sts)}")
        _, lpc = link_read(link, 0x21, 4)
        _, cnt = link_read(link, 0x2C, 4)
        print(f"FPGA design {r[4]}.{r[5]}.{r[6]}  board {r[7]}  features {r[8] | r[9] << 8:04X}")
        print(f"LRESET# {'high' if lpc[0] & 1 else 'low'}  resets {lpc[3]}  "
              f"memory reads since reset {struct.unpack('<I', cnt)[0]}")
        print(sts_text(sts))
    elif a.what == "read":
        sts, r = link_read(link, a.addr, a.len or 1, a.space)
        for off in range(0, len(r), 16):
            print(f"{a.addr + off:06X}  {r[off:off + 16].hex(' ').upper()}")
        print(sts_text(sts))
    elif a.what == "write":
        if not a.data:
            sys.exit("link write: data required")
        print(sts_text(link_write(link, a.addr, bytes.fromhex(a.data), a.space)))
    elif a.what == "irq":
        v = struct.unpack("<18I", link.request(LINK_IRQ))
        print(f"interrupts {v[0]}, spurious {v[1]}")
        for i, name in enumerate(IRQ_BITS):
            if v[2 + i]:
                print(f"  {name:14} {v[2 + i]}")
    elif a.what == "probe":
        r = link.request(LINK_PROBE)
        lv = r[0]
        print("MISO with CS# high, ESP32 pull-up   (expect 1):", lv & 1)
        print("MISO with CS# high, ESP32 pull-down (expect 0):", lv >> 1 & 1)
        print("MISO with CS# low, FPGA driving     (expect STS bit 7 = 0):", lv >> 2 & 1)
        print(f"bit-banged STS {r[1]:02X}  ID {r[2:6].hex(' ').upper()}  (expect 4F 58 46 47)")
    elif a.what == "test":
        t0 = time.time()
        for i in range(a.count):
            v = os.urandom(4)
            link_write(link, 0x0C, v)
            _, r = link_read(link, 0x0C, 4)
            if r != v:
                raise LinkError(f"pass {i}: wrote {v.hex(' ')}, read {r.hex(' ')}")
            if i % 20 == 0:
                print(f"\r{i}/{a.count}", end="", flush=True)
        print(f"\r{a.count} scratch write/read passes ok in {time.time() - t0:.1f} s")


def cmd_usb(link, a):
    if a.what == "start":
        hub = a.profile == "hub"
        link.request(USB_START, bytes([1 if hub else 0]))
        print("started: " + ("hub, test gamepad on port 1" if hub else "test gamepad") + ", attached")
    elif a.what == "stop":
        link.request(USB_STOP)
        print("stopped, detached")
    elif a.what in ("plug", "unplug"):
        link.request(USB_PLUG, bytes([a.what == "plug"]))
        print(f"test gamepad {a.what}ged, hub port 1")
    else:
        r = link.request(USB_STATUS)
        running, state = r[0], r[1]
        v = struct.unpack_from("<9I", r, 2)
        addr = r[38:46]
        print(f"{'running' if running else 'stopped'}, gamepad {USB_STATES[state] if state < 4 else state}"
              + (f", hub {USB_STATES[r[46]] if r[46] < 4 else r[46]}" if len(r) > 46 and r[46] != 0xFF else ""))
        print(f"reports taken {v[0]}, OUT packets {v[1]}")
        print(f"events {v[2]}: bus resets {v[3]}, SETUPs {v[4]}, IN done {v[5]}, OUT done {v[6]}, "
              f"suspends {v[7]}, link errors {v[8]}")
        used = [f"{d}:{x}" for d, x in enumerate(addr) if x != 0xFF]
        print("addresses (device:address)", " ".join(used) if used else "none")


def smb_read(link, addr, reg, width=1):
    r = link.request(SMBUS_READ, bytes([addr, reg, width]))
    return r[0] if width == 1 else struct.unpack("<H", r)[0]


def cmd_smbus(link, a):
    width = 2 if a.word else 1
    if a.what == "read":
        print(f"0x{smb_read(link, a.addr, a.reg, width):0{width * 2}X}")
    else:
        if a.value is None:
            sys.exit("smbus write: value required")
        link.request(SMBUS_WRITE, bytes([a.addr, a.reg, width])
                     + a.value.to_bytes(width, "little"))
        print("ok")


def cmd_xbox(link, a):
    if a.what == "status":
        for name, reg in [("tray state", 0x03), ("AV pack", 0x04), ("CPU temp", 0x09),
                          ("board temp", 0x0A)]:
            try:
                print(f"{name:11} 0x{smb_read(link, SMC, reg):02X}")
            except LinkError as e:
                print(f"{name:11} {e}")
        return
    link.request(SMC_POWER, bytes([{"reset": 0x01, "cycle": 0x40, "off": 0x80}[a.what]]))
    print(f"SMC: {a.what}")


# ---------------------------------------------------------------- programmer protocol
PROG_MAGIC = 0x5046584F
PROG_STATUS = ["Ok", "ConsoleOn", "ProgrammerBusy", "LayoutInvalid", "NotStarted", "BadSize",
               "BadOffset", "BadCommand", "FlashError", "CrcMismatch", "NotFound", "Protected",
               "NoSpace", "ConsoleOff", "SmbusError", "Fragmented"]
MODE_CONTROL, MODE_UPLOAD, MODE_DOWNLOAD, MODE_ROM = 1, 2, 3, 4
CTL_GET_INFO, CTL_REBOOT, CTL_SMBUS_READ, CTL_SMBUS_WRITE, CTL_BOOT, CTL_CONFIRM = 0, 1, 2, 3, 4, 5
ROM_LIST, ROM_GET, ROM_DELETE, ROM_RENAME, ROM_SET_USER, ROM_COMPACT = 0, 1, 2, 3, 4, 5
UP_BEGIN, UP_WRITE, UP_COMMIT, UP_ABORT = 0, 1, 2, 3
DOWN_BEGIN, DOWN_READ, DOWN_END = 0, 1, 2
ROLES = ["user", "loader", "system", "diag"]
UPLOAD_TYPES = {"user": 1, "loader": 2, "system": 3, "diag": 4}
ROM_ENTRY = struct.Struct("<BBBBIIIHH33s")
ROM_FLAGS = [(0x01, "current"), (0x02, "trial"), (0x04, "failed-boot"), (0x08, "user")]


def prog(link, mode, action, payload=b"", timeout=2.0):
    cmd = (mode & 0x0F) | (action << 4)
    hdr = struct.pack("<IBBHI", PROG_MAGIC, cmd, 0, len(payload),
                      zlib.crc32(payload) if payload else 0)
    r = link.request(PROG_XFER, hdr + struct.pack("<I", zlib.crc32(hdr)) + payload,
                     timeout=timeout)
    magic, rcmd, status, n, pcrc, hcrc = struct.unpack_from("<IBBHII", r)
    body = r[16:16 + n]
    if magic != PROG_MAGIC or hcrc != zlib.crc32(r[:12]) or rcmd != cmd or len(body) != n             or (n and pcrc != zlib.crc32(body)):
        raise LinkError("prog: malformed reply")
    if status:
        name = PROG_STATUS[status] if status < len(PROG_STATUS) else f"status {status}"
        raise ProgError(status, f"prog: {name}")
    return body


class ProgError(LinkError):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status = status


def rom_compact(link, size):
    t0 = time.time()
    while True:
        # Each step copies up to 512 K
        moved, left = prog(link, MODE_ROM, ROM_COMPACT, struct.pack("<I", size), timeout=60)
        if moved == 0xFF:
            break
        print(f"moved entry {moved} ({left} to go)")
    print(f"{size // 1024} K free in {time.time() - t0:.1f} s")


def parse_size(t):
    t = t.upper()
    mul = {"K": 1024, "M": 1024 * 1024}.get(t[-1:], 1)
    return int(t.rstrip("KM"), 0) * mul


def cmd_nor(link, a):
    if a.what == "info":
        r = link.request(NOR_INFO)
        print(f"flash {'reachable' if r[0] else 'not reachable'}  ID: {r[1:4].hex(' ').upper()}")
        return
    t0 = time.time()
    if a.what == "read":
        out = bytearray()
        while len(out) < a.len:
            n = min(4096, a.len - len(out))
            out += link.request(NOR_READ, struct.pack("<IH", a.addr + len(out), n), timeout=15)
            print(f"\rread {len(out)}/{a.len}", end="", flush=True)
        print()
        with open(a.file, "wb") as f:
            f.write(out)
        print(f"read {len(out)} bytes from 0x{a.addr:06X} to {a.file} in {time.time() - t0:.1f} s")
    elif a.what == "erase":
        for off in range(0, a.len, 0x10000):
            n = min(0x10000, a.len - off)
            link.request(NOR_ERASE, struct.pack("<II", a.addr + off, n), timeout=15)
        print(f"erased {a.len} bytes at 0x{a.addr:06X} in {time.time() - t0:.1f} s")
    elif a.what == "crc":
        crc = Reader(link.request(NOR_CRC, struct.pack("<II", a.addr, a.len), timeout=15)).u32()
        print(f"CRC {crc:08X} over {a.len} bytes at 0x{a.addr:06X} ({time.time() - t0:.2f} s)")
    elif a.what == "write":
        data = open(a.file, "rb").read()
        if a.addr & 0xFFF:
            raise LinkError("--addr must be a multiple of 4 KB")
        span = (len(data) + 0xFFF) & ~0xFFF
        for off in range(0, span, 0x10000):
            n = min(0x10000, span - off)
            link.request(NOR_ERASE, struct.pack("<II", a.addr + off, n), timeout=15)
            print(f"\rerased {off + n}/{span}", end="", flush=True)
        print()
        for off in range(0, len(data), 4096):
            link.request(NOR_WRITE, struct.pack("<I", a.addr + off) + data[off:off + 4096],
                         timeout=15)
            print(f"\rwritten {min(off + 4096, len(data))}/{len(data)}", end="", flush=True)
        print()
        crc = Reader(link.request(NOR_CRC, struct.pack("<II", a.addr, len(data)),
                                  timeout=15)).u32()
        want = zlib.crc32(data)
        if crc != want:
            raise LinkError(f"verify failed: flash CRC {crc:08X}, file {want:08X}")
        print(f"verified {len(data)} bytes at 0x{a.addr:06X} (CRC {crc:08X}) "
              f"in {time.time() - t0:.1f} s")


def cmd_prog(link, a):
    if a.what == "info":
        f = struct.unpack_from("<H3B3BBBB6BBBBIII", prog(link, MODE_CONTROL, CTL_GET_INFO)
                               .ljust(32, bytes(1)))
        entry = lambda v: "-" if v == 0xFF else str(v)
        print(f"protocol     {f[0]}")
        print(f"firmware     {f[1]}.{f[2]}.{f[3]}")
        print(f"FPGA         {f[4]}.{f[5]}.{f[6]}, board {f[7]}")
        print(f"console      {'on' if f[8] else 'off'}, boot flags 0x{f[9]:02X}")
        print("entries      active {} armed {} user {} loader {} system {} diag {}"
              .format(*map(entry, f[10:16])))
        print(f"catalogue    {f[16]} entries, {f[17]} slots, {f[18]} free")
        print(f"flash        {f[19]} bytes, retail tables {f[20]} bytes (CRC {f[21]:08X})")
    elif a.what == "smbus-read":
        r = prog(link, MODE_CONTROL, CTL_SMBUS_READ, bytes([a.args[0], a.args[1], 0, 0]))
        print(f"0x{r[2]:02X}")
    elif a.what == "smbus-write":
        if len(a.args) < 3:
            sys.exit("prog smbus-write: ADDR REG VALUE")
        prog(link, MODE_CONTROL, CTL_SMBUS_WRITE, bytes([a.args[0], a.args[1], a.args[2], 0]))
        print("ok")
    elif a.what == "confirm":
        prog(link, MODE_CONTROL, CTL_CONFIRM)
        print("trial boot confirmed")


def rom_entries(body):
    for k in range(0, len(body) - ROM_ENTRY.size + 1, ROM_ENTRY.size):
        yield ROM_ENTRY.unpack_from(body, k)


def rom_name(raw):
    return raw.split(b"\0", 1)[0].decode(errors="replace")


def cmd_rom(link, a):
    args = a.args
    need = lambda n, usage: len(args) >= n or sys.exit(f"rom {a.what}: {usage}")
    if a.what == "list":
        rows, first = [], 0
        while True:
            body = prog(link, MODE_ROM, ROM_LIST, bytes([first]))
            got = list(rom_entries(body))
            rows += got
            if len(got) < 4096 // ROM_ENTRY.size or not got:
                break
            first = got[-1][0] + 1
        if not rows:
            print("catalogue empty")
        for (eid, role, bank, flags, size, crc, src, ver, _, name) in rows:
            fl = ",".join(n for b, n in ROM_FLAGS if flags & b)
            print(f"{eid:3}  {ROLES[role] if role < 4 else role:6}  slot {bank & 0x3F:2}  "
                  f"{size // 1024:5} K  CRC {crc:08X}  {rom_name(name):32} {fl}")
    elif a.what == "upload":
        need(1, "FILE [--name N] [--type T] [--user] [--boot]")
        data = open(args[0], "rb").read()
        name = (a.name or os.path.splitext(os.path.basename(args[0]))[0]).encode()[:32]
        flags = (0x01 if a.trial else 0) | (0x02 if a.user else 0) | (0x04 if a.boot else 0)
        begin = struct.pack("<BBHI33s", UPLOAD_TYPES[a.type], 0, 0, len(data), name)
        try:
            prog(link, MODE_UPLOAD, UP_BEGIN, begin)
        except ProgError as e:
            if e.status != PROG_STATUS.index("Fragmented"):
                raise
            if not a.compact:
                sys.exit("no free run of that size until entries are moved: "
                         "add --compact, or run rom compact first")
            rom_compact(link, len(data))
            prog(link, MODE_UPLOAD, UP_BEGIN, begin)
        t0 = time.time()
        try:
            for off in range(0, len(data), 4092):
                prog(link, MODE_UPLOAD, UP_WRITE, struct.pack("<I", off) + data[off:off + 4092])
                print(f"\rwritten {min(off + 4092, len(data))}/{len(data)}", end="", flush=True)
            print()
            body = prog(link, MODE_UPLOAD, UP_COMMIT,
                        struct.pack("<IIHBB", zlib.crc32(data), 0, 0, flags, 0))
        except LinkError:
            try:
                prog(link, MODE_UPLOAD, UP_ABORT)
            except LinkError:
                pass
            raise
        e = ROM_ENTRY.unpack_from(body)
        print(f"entry {e[0]}: slot {e[2] & 0x3F}, {e[4] // 1024} K, CRC {e[5]:08X}, "
              f"{time.time() - t0:.1f} s"
              + (", armed on trial" if a.trial else ""))
        if a.reset:
            link.request(SMC_POWER, bytes([0x01]))
            print("console reset")
    elif a.what == "download":
        need(2, "ID|active OUT")
        begin = bytes([2, 0]) if args[0] == "active" else bytes([1, int(args[0], 0)])
        size, crc = struct.unpack("<II", prog(link, MODE_DOWNLOAD, DOWN_BEGIN, begin))
        out = bytearray()
        while len(out) < size:
            n = min(4096, size - len(out))
            out += prog(link, MODE_DOWNLOAD, DOWN_READ, struct.pack("<II", len(out), n))
            print(f"\rread {len(out)}/{size}", end="", flush=True)
        print()
        prog(link, MODE_DOWNLOAD, DOWN_END)
        if zlib.crc32(out) != crc:
            raise LinkError(f"download CRC {zlib.crc32(out):08X}, expected {crc:08X}")
        open(args[1], "wb").write(out)
        print(f"{size} bytes to {args[1]} (CRC {crc:08X})")
    elif a.what == "delete":
        need(1, "ID")
        prog(link, MODE_ROM, ROM_DELETE, bytes([int(args[0], 0)]))
        print("deleted")
    elif a.what == "rename":
        need(2, "ID NAME")
        prog(link, MODE_ROM, ROM_RENAME, struct.pack("<B33s", int(args[0], 0),
                                                    args[1].encode()[:32]))
        print("renamed")
    elif a.what == "user":
        need(1, "ID|none")
        prog(link, MODE_ROM, ROM_SET_USER, bytes([0xFF if args[0] == "none" else int(args[0], 0)]))
        print("power-button ROM set")
    elif a.what == "compact":
        need(1, "SIZE (256K, 512K or 1M)")
        rom_compact(link, parse_size(args[0]))
    elif a.what == "boot":
        need(1, "ID [--reset]")
        prog(link, MODE_CONTROL, CTL_BOOT, bytes([int(args[0], 0), 1 if a.reset else 0]))
        print("armed" + (", console reset" if a.reset else " for the next reset"))


def cmd_log(link, a):
    level = LEVELS.index(a.level)
    link.request(LOG_LEVEL, bytes([level]) + (a.tag or "").encode())
    print(f"log level {a.level} for {a.tag or 'all tags'}")


def main():
    p = argparse.ArgumentParser(description="OpenOX PC tool")
    p.add_argument("-p", "--port", default=os.environ.get("OXCTL_PORT"))
    p.add_argument("-b", "--baud", type=int, default=921600)
    p.add_argument("-v", "--verbose", action="store_true", help="also print logs and events")
    sub = p.add_subparsers(dest="group", required=True)
    s = sub.add_parser("sys")
    s.add_argument("what", choices=["ping", "version", "status", "reboot"])
    s = sub.add_parser("log")
    s.add_argument("op", choices=["level"])
    s.add_argument("level", choices=LEVELS)
    s.add_argument("tag", nargs="?")
    s = sub.add_parser("fpga")
    s.add_argument("what", choices=["status", "id", "probe", "read", "write", "release"])
    s.add_argument("file", nargs="?")
    s.add_argument("--addr", type=lambda x: int(x, 0), default=0)
    s.add_argument("--len", type=lambda x: int(x, 0), default=0)
    s = sub.add_parser("smbus")
    s.add_argument("what", choices=["read", "write"])
    s.add_argument("addr", type=lambda x: int(x, 0))
    s.add_argument("reg", type=lambda x: int(x, 0))
    s.add_argument("value", nargs="?", type=lambda x: int(x, 0))
    s.add_argument("--word", action="store_true")
    s = sub.add_parser("xbox")
    s.add_argument("what", choices=["status", "reset", "cycle", "off"])
    s = sub.add_parser("link")
    s.add_argument("what", choices=["info", "probe", "test", "read", "write", "irq"])
    s.add_argument("addr", nargs="?", type=lambda x: int(x, 0), default=0)
    s.add_argument("data", nargs="?")
    s.add_argument("--len", type=lambda x: int(x, 0), default=0)
    s.add_argument("--space", type=int, default=0)
    s.add_argument("--count", type=int, default=200)
    s.add_argument("--mhz", type=float, help="set the link clock first (kept until reboot)")
    s = sub.add_parser("nor")
    s.add_argument("what", choices=["info", "read", "write", "erase", "crc"])
    s.add_argument("file", nargs="?")
    s.add_argument("--addr", type=lambda x: int(x, 0), default=0)
    s.add_argument("--len", type=lambda x: int(x, 0), default=0)
    s = sub.add_parser("prog")
    s.add_argument("what", choices=["info", "smbus-read", "smbus-write", "confirm"])
    s.add_argument("args", nargs="*", type=lambda x: int(x, 0))
    s = sub.add_parser("rom")
    s.add_argument("what", choices=["list", "upload", "download", "delete", "rename", "user",
                                    "boot", "compact"])
    s.add_argument("args", nargs="*")
    s.add_argument("--name")
    s.add_argument("--type", choices=list(UPLOAD_TYPES), default="user")
    s.add_argument("--user", action="store_true", help="upload: make it the power-button ROM")
    s.add_argument("--boot", action="store_true", help="upload: boot it from the next reset")
    s.add_argument("--compact", action="store_true",
                   help="upload: move entries if there is room but no free run")
    s.add_argument("--trial", action="store_true",
                   help="upload: a Loader or System, current only once it confirms")
    s.add_argument("--reset", action="store_true", help="boot, upload: reset the console now")
    s = sub.add_parser("usb")
    s.add_argument("what", choices=["start", "stop", "status", "plug", "unplug"])
    s.add_argument("profile", nargs="?", choices=["pad", "hub"], default="pad",
                   help="start: the test gamepad alone, or behind a 4-port hub")
    s = sub.add_parser("monitor")
    s.add_argument("--reset", action="store_true", help="reset the ESP32 after opening")
    a = p.parse_args()
    if not a.port:
        sys.exit("no port: use -p COMx or set OXCTL_PORT")

    quiet = not a.verbose and a.group != "monitor"
    show_text = lambda b: print(b.decode(errors="replace"), end="")
    link = Link(a.port, a.baud,
                on_log=(lambda s: None) if quiet else None,
                on_event=(lambda i, d: None) if quiet else None,
                on_text=(lambda b: None) if quiet else show_text)
    try:
        if a.group == "sys":
            cmd_sys(link, a)
        elif a.group == "fpga":
            if a.what in ("read", "write") and not a.file:
                sys.exit(f"fpga {a.what}: file required")
            cmd_fpga(link, a)
        elif a.group == "smbus":
            cmd_smbus(link, a)
        elif a.group == "xbox":
            cmd_xbox(link, a)
        elif a.group == "link":
            cmd_link(link, a)
        elif a.group == "nor":
            if a.what in ("read", "write") and not a.file:
                sys.exit(f"nor {a.what}: file required")
            cmd_nor(link, a)
        elif a.group == "rom":
            cmd_rom(link, a)
        elif a.group == "prog":
            if a.what.startswith("smbus") and len(a.args) < 2:
                sys.exit(f"prog {a.what}: ADDR REG required")
            cmd_prog(link, a)
        elif a.group == "log":
            cmd_log(link, a)
        elif a.group == "usb":
            cmd_usb(link, a)
        elif a.group == "monitor":
            if a.reset:
                link.reset()
            print("monitoring (Ctrl+C to stop)")
            wait_forever()
    except LinkError as e:
        sys.exit(str(e))
    finally:
        link.close()


if __name__ == "__main__":
    main()
