#!/usr/bin/env python3
"""Viewer for the lpc_probe FPGA design (boards/upduino31/top_lpc_probe.v).

Opens the probe's serial port, asks for the last report (sends Enter), then keeps
listening: every Xbox power-on produces a report, which is decoded and printed here
and appended to a log file (one JSON object per line) for later comparison.

    py -3 tools/lpc_probe_view.py COM11
    py -3 tools/lpc_probe_view.py COM11 --log my_xbox_v1.4.jsonl
    py -3 tools/lpc_probe_view.py COM11 --status     # live input levels, for wiring checks
    py -3 tools/lpc_probe_view.py COM11 --iolog      # for the lpc_iolog design (921600 baud)

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


CSTATE = {0: "off", 1: "capturing", 2: "done", 3: "?"}


def render_status(v):
    lad = (v >> 12) & 0xF
    return (f"3V3 sense raw={(v >> 9) & 1} filtered={(v >> 8) & 1} toggles/s={(v >> 16) & 0xFF:<3} | "
            f"LCLK {'RUNNING' if (v >> 7) & 1 else 'stopped'} | LRESET#={(v >> 10) & 1} "
            f"LFRAME#={(v >> 11) & 1} LAD={lad:04b} | state={CSTATE[(v >> 5) & 3]}"
            f"{' report-stored' if (v >> 4) & 1 else ''}")


IOLOG_RE = re.compile(r"^([RD]) ([0-9A-F]{8}|[0-9A-F]{16})$")
EVENTS = {1: "*** 3.3V UP (power on)", 2: "*** 3.3V DOWN (power off)",
          3: "--- LRESET# released", 4: "--- LRESET# asserted"}


def run_iolog(ser, args):
    """lpc_iolog stream: decode records, print, and append CSV."""
    csv_path = args.log if args.log.endswith(".csv") else "lpc_iolog.csv"
    print(f"lpc_iolog: logging to {csv_path}; Ctrl+C to quit.")
    print("     t (since power-on)   type     addr        data")
    t_on = None
    prev_t = None
    wraps = 0
    buf = b""
    with open(csv_path, "a") as csv:
        csv.write("# t_us_abs,t_ms_since_power_on,type,addr,data,flags\n")
        while True:
            buf += ser.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                m = IOLOG_RE.match(line)
                if not m:
                    if line:
                        print(f"[probe] {line}")
                    continue
                if m.group(1) == "D":
                    print(f"!!! {int(m.group(2), 16)} records dropped (FIFO full)")
                    continue
                r = int(m.group(2), 16)
                t = r >> 36
                if prev_t is not None and t < prev_t:
                    wraps += 1
                prev_t = t
                t_abs = t + (wraps << 28)
                ct = (r >> 32) & 0xF
                if ct == 0xF:
                    code = r & 0xFFFF
                    if code == 1:
                        t_on = t_abs
                    rel = "" if t_on is None else f"{(t_abs - t_on) / 1000:12.3f} ms"
                    print(f"{rel:>21}   {EVENTS.get(code, f'event {code}')}")
                    csv.write(f"{t_abs},{'' if t_on is None else (t_abs - t_on) / 1000},EVENT,{code},,\n")
                    continue
                if (ct >> 1) == 3:                       # memory write
                    name, addr, data, flags = "MEM WR", f"{r & 0xFFFFFF:06X}", (r >> 24) & 0xFF, ""
                else:
                    name = {0: "IO RD", 1: "IO WR", 4: "DMA RD", 5: "DMA WR"}.get(ct >> 1, f"ct={ct:X}")
                    addr, data = f"{r & 0xFFFF:04X}", (r >> 16) & 0xFF
                    flags = "NO-RESPONSE/ERR" if (r >> 31) & 1 else ""
                arrow = "->" if "RD" in name else "<-"
                rel = "" if t_on is None else f"{(t_abs - t_on) / 1000:12.3f} ms"
                print(f"{rel:>21}   {name:<8} {addr:<8} {arrow} {data:02X}  {flags}")
                csv.write(f"{t_abs},{'' if t_on is None else (t_abs - t_on) / 1000},{name},{addr},{data:02X},{flags}\n")
            csv.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="serial port, e.g. COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default="lpc_probe_log.jsonl", help="JSON-lines log file")
    ap.add_argument("--iolog", action="store_true",
                    help="decode the lpc_iolog design's stream (sets 921600 baud)")
    ap.add_argument("--status", action="store_true",
                    help="poll live input levels twice a second instead of waiting for reports")
    args = ap.parse_args()

    if args.iolog and args.baud == 115200:
        args.baud = 921600
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        sys.exit(f"Can't open {args.port}: {e}\n(Is another terminal using it?)")

    if args.iolog:
        try:
            run_iolog(ser, args)
        except KeyboardInterrupt:
            pass
        ser.close()
        return

    if args.status:
        import time
        print("Live input status (Ctrl+C to quit). Expected: Xbox off -> raw=0, toggles 0; "
              "Xbox on -> raw=1, LCLK RUNNING, LRESET#=1")
        try:
            while True:
                ser.reset_input_buffer()
                ser.write(b"s")
                line = ser.readline().decode("ascii", "replace").strip()
                m = LINE_RE.match(line)
                if m and m.group(1) == "3F":
                    print(render_status(int(m.group(2), 16)), flush=True)
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        ser.close()
        return

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
                if idx == 0x3F:
                    print(f"[status] {render_status(val)}")
                    continue
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
