#!/usr/bin/env python3
"""OpenOX PC tool.

    py -3 tools/oxctl.py -p COM12 sys version
    py -3 tools/oxctl.py -p COM12 sys status
    py -3 tools/oxctl.py -p COM12 sys ping
    py -3 tools/oxctl.py -p COM12 sys reboot
    py -3 tools/oxctl.py -p COM12 log level debug [tag]
    py -3 tools/oxctl.py -p COM12 monitor            # print logs and events until Ctrl+C
    py -3 tools/oxctl.py -p COM12 monitor --reset    # reset the ESP32 first (boot log)

The port can also come from the OXCTL_PORT environment variable.
"""
import argparse
import os
import sys

from oxlink import Link, LinkError, Reader, wait_forever

SYS_PING, SYS_VERSION, SYS_STATUS, SYS_REBOOT = 0x0000, 0x0001, 0x0002, 0x0003
LOG_LEVEL = 0x0100

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
