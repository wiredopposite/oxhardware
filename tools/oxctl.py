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
LINK_READ, LINK_WRITE, LINK_PROBE, LINK_CLOCK = 0x0300, 0x0301, 0x0302, 0x0303
SMBUS_READ, SMBUS_WRITE, SMC_POWER = 0x0400, 0x0401, 0x0402
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
    s.add_argument("what", choices=["info", "probe", "test", "read", "write"])
    s.add_argument("addr", nargs="?", type=lambda x: int(x, 0), default=0)
    s.add_argument("data", nargs="?")
    s.add_argument("--len", type=lambda x: int(x, 0), default=0)
    s.add_argument("--space", type=int, default=0)
    s.add_argument("--count", type=int, default=200)
    s.add_argument("--mhz", type=float, help="set the link clock first (kept until reboot)")
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
        elif a.group == "log":
            cmd_log(link, a)
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
