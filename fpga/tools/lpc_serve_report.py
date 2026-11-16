#!/usr/bin/env python3
"""Read and decode the lpc_serve report; check served bytes against the ROM image.

    py -3 tools/lpc_serve_report.py COM11 ../dumps/<image>.bin
    py -3 tools/lpc_serve_report.py COM11 --trace build/trace.txt   # dump the LA capture
"""
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: py -3 -m pip install pyserial")

ROM_BASE = 0x40000
NAMES = {
    0x01: "reads served", 0x02: "flash restarts", 0x03: "SYNC timeouts",
    0x04: "last SYNC wait (clk)", 0x05: "max SYNC wait (clk)", 0x06: "LRESET# releases",
    0x07: "LCLK/65536 ticks", 0x08: "LFRAME# pulled (clk)", 0x09: "..sensed HIGH, first 4 clk",
    0x0A: "..sensed HIGH, later", 0x0B: "LFRAME# LOW 1 clk after release",
    0x0C: "LFRAME# LOW 2 clk after release", 0x0D: "LAD driven (clk)",
    0x0E: "LAD read back different", 0x0F: "LRESET# glitches rejected",
    0x10: "fail-safe recoveries", 0x11: "reads outside FFxxxxxx", 0x12: "boot holds released",
    0x13: "reads since last reset release", 0x24: "trace frozen",
}

STATES = ["IDLE", "CT", "ADDR", "TAR", "SYNC", "DLO", "DHI", "TAR1", "REL",
          "SKIP1", "SKSYNC", "SKIP2", "s12", "s13", "s14", "s15"]

def trace(port, out):
    s = serial.Serial(port, 115200, timeout=0.5)
    s.reset_input_buffer()
    s.write(b"t")
    t, buf = time.time(), b""
    while time.time() - t < 3:
        buf += s.read(8192)
    s.close()
    rows = re.findall(rb"([0-9A-F]{3}) ([0-9A-F]{4})\r?\n", buf)
    if len(rows) < 500:
        sys.exit(f"trace not available ({len(rows)} lines) - not frozen yet?")
    with open(out, "w") as f:
        f.write("idx  LAD LF# LADoe LFoe RST# hold state\n")
        for i, v in rows:
            v = int(v, 16)
            f.write(f"{int(i, 16):3d}  {v & 0xF:04b}  {(v >> 4) & 1}    {(v >> 5) & 1}     "
                    f"{(v >> 6) & 1}    {(v >> 7) & 1}    {(v >> 12) & 1}   {STATES[(v >> 8) & 0xF]}\n")
    print(f"{len(rows)} samples -> {out} (trigger = first memory read outside FFxxxxxx, 64 samples before the end)")

def main():
    if len(sys.argv) == 4 and sys.argv[2] == "--trace":
        return trace(sys.argv[1], sys.argv[3])
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = open(sys.argv[2], "rb").read()
    s = serial.Serial(sys.argv[1], 115200, timeout=0.3)
    s.reset_input_buffer()
    s.write(b"\r")
    t, buf = time.time(), b""
    while time.time() - t < 2.5:
        buf += s.read(4096)
    s.close()
    w = {int(a, 16): int(b, 16) for a, b in re.findall(rb"([0-9A-F]{2}) ([0-9A-F]{8})", buf)}
    if w.get(0) != 0x4C530002:
        sys.exit(f"no lpc_serve v2 report (got {len(w)} words)")
    for i, name in sorted(NAMES.items()):
        print(f"  {name:<34} {w.get(i, 0):>12}")

    def entry(i):
        v = w.get(i, 0)
        addr, data = v & 0xFFFFFF, v >> 24
        if v == 0:
            return "  (empty)"
        off = addr - ROM_BASE
        want = rom[off % len(rom)] if 0 <= off else None
        ok = "ok" if want == data else f"MISMATCH (file has {want:02X})"
        return f"  flash {addr:06X}  rom+{off:05X}  served {data:02X}  {ok}"

    print("First 8 reads after the last LRESET# release:")
    for i in range(0x14, 0x1C):
        print(entry(i))
    print("Last 8 reads before the last LRESET# assertion:")
    for i in range(0x1C, 0x24):
        print(entry(i))

if __name__ == "__main__":
    main()
