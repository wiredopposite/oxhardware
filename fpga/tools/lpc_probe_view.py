#!/usr/bin/env python3
"""Viewer for the lpc_probe FPGA design (boards/upduino31/top_lpc_probe.v).

Opens the probe's serial port, asks for the last report (sends Enter), then keeps
listening: every Xbox power-on produces a report, which is decoded and printed here
and appended to a log file (one JSON object per line) for later comparison.

    py -3 tools/lpc_probe_view.py COM11
    py -3 tools/lpc_probe_view.py COM11 --log my_xbox_v1.4.jsonl

Needs pyserial (pip install pyserial). Ctrl+C to quit. Close any other terminal on
the same COM port first - Windows allows only one program per port.
"""
import argparse
import datetime
import json
import re
import sys

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: py -3 -m pip install pyserial")

LINE_RE = re.compile(r"^([0-9A-F]{2}) ([0-9A-F]{8})$")
N_WORDS = 0x30
NEVER = 0xFFFFFFFF
MAGIC = 0x4C500001


def ms(us):
    return "never" if us == NEVER else f"{us / 1000:9.3f} ms"


def cycle_name(start, ct):
    if start == 0x0:
        return {0: "IO RD", 1: "IO WR", 2: "MEM RD", 3: "MEM WR",
                4: "DMA RD", 5: "DMA WR"}.get(ct >> 1, f"RSVD ct={ct:X}")
    if start == 0xD:
        return f"FWH RD idsel={ct:X}"
    if start == 0xE:
        return f"FWH WR idsel={ct:X}"
    return f"START={start:X} ct={ct:X}"


def render(w):
    out = []
    out.append(f"=== Xbox power-on #{w[1]} ===")
    out.append(f"  LCLK first activity    : {ms(w[2])}")
    n_rst = w[7]
    for i in range(min(n_rst, 4)):
        out.append(f"  LRESET# release #{i + 1}     : {ms(w[3 + i])}")
    if n_rst == 0:
        out.append("  LRESET# release        : never")
    elif n_rst > 4:
        out.append(f"  (LRESET# released {n_rst} times in total)")
    out.append(f"  First LPC cycle        : {ms(w[8])}")
    if w[8] != NEVER and n_rst:
        out.append(f"    -> after 1st LRESET# : {(w[8] - w[3]) :>8} us")
    out.append(f"  Cycles total           : {w[9]}  "
               f"(mem rd {w[10]}, mem wr {w[11]}, io rd {w[12]}, io wr {w[13]}, other {w[14]})")
    if w[15]:
        out.append(f"  LCLK (last 1.024 ms)   : {w[15] * 16 / 1.024 / 1000:.2f} MHz")
    n_rec = min(w[9], 16)
    if n_rec:
        out.append(f"  First {n_rec} cycles:")
        for i in range(n_rec):
            hdr, addr = w[0x10 + 2 * i], w[0x11 + 2 * i]
            start, ct, t = (hdr >> 28) & 0xF, (hdr >> 24) & 0xF, hdr & 0xFFFFFF
            name = cycle_name(start, ct)
            a = f"{addr & 0xFFFF:04X}" if name.startswith("IO") else f"{addr:08X}"
            out.append(f"    {t / 1000:9.3f} ms  {name:<16} {a}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="serial port, e.g. COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default="lpc_probe_log.jsonl", help="JSON-lines log file")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        sys.exit(f"Can't open {args.port}: {e}\n(Is another terminal using it?)")

    print(f"Listening on {args.port} - power the Xbox on/off; Ctrl+C to quit. Log: {args.log}")
    ser.write(b"\r")             # prints the last report (or the banner)
    words = {}
    buf = b""
    try:
        while True:
            buf += ser.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if not line:
                    continue
                m = LINE_RE.match(line)
                if not m:
                    print(f"[probe] {line}")
                    continue
                idx, val = int(m.group(1), 16), int(m.group(2), 16)
                if idx == 0:
                    words = {}
                words[idx] = val
                if idx == N_WORDS - 1:
                    if len(words) == N_WORDS and words[0] == MAGIC:
                        w = [words[i] for i in range(N_WORDS)]
                        print(render(w))
                        with open(args.log, "a") as f:
                            f.write(json.dumps({"time": datetime.datetime.now().isoformat(
                                timespec="seconds"), "words": w}) + "\n")
                    else:
                        print("[viewer] incomplete report, ignored")
                    words = {}
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
