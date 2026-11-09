"""PC side of the OpenOX ESP32 protocol (docs/03-esp32.md §12).

COBS-encoded frames delimited by 0x00:
    type (1) | seq (1) | len (2, LE) | payload | CRC-32 (4, LE, over type..payload)
"""
import struct
import threading
import time
import zlib

import serial

REQUEST, OK, ERR, EVENT, LOG = 1, 2, 3, 4, 5
ERRORS = {1: "unknown command", 2: "bad arguments", 3: "busy", 4: "failed"}


def cobs_encode(data):
    out = bytearray(b"\x00")
    code_at, code = 0, 1
    for b in data:
        if b == 0:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
            continue
        out.append(b)
        code += 1
        if code == 0xFF:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
    out[code_at] = code
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            return None
        out += data[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


class LinkError(Exception):
    pass


class Link:
    """Request/response over the ESP32's UART0. Log and event frames go to callbacks;
    bytes that aren't valid frames (boot ROM text, panic dumps) go to on_text."""

    def __init__(self, port, baud=921600, on_log=None, on_event=None, on_text=None):
        # DTR/RTS drive the ESP32's EN and IO0 through the auto-reset circuit: set them
        # inactive before opening so opening the port doesn't reset the board
        self.ser = serial.Serial(None, baud, timeout=0.05)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.on_log = on_log or (lambda s: print(s, end="" if s.endswith("\n") else "\n"))
        self.on_event = on_event or (lambda i, d: print(f"[event {i:04x}] {d.hex()}"))
        self.on_text = on_text or (lambda b: None)
        self.seq = 0
        self.lock = threading.Lock()
        self.pending = {}
        self.stop = False
        self.rx = threading.Thread(target=self._rx, daemon=True)
        self.rx.start()

    def reset(self):
        """Pulse EN low through RTS (auto-reset circuit), IO0 left high: normal boot."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False

    def close(self):
        self.stop = True
        self.rx.join()
        self.ser.close()

    def _rx(self):
        buf = bytearray()
        while not self.stop:
            data = self.ser.read(4096)
            for b in data:
                if b:
                    buf.append(b)
                    continue
                if buf:
                    self._frame(bytes(buf))
                buf.clear()

    def _frame(self, raw):
        f = cobs_decode(raw)
        if f is None or len(f) < 8 or struct.unpack_from("<H", f, 2)[0] != len(f) - 8 \
                or zlib.crc32(f[:-4]) != struct.unpack_from("<I", f, len(f) - 4)[0]:
            self.on_text(raw)
            return
        ftype, seq, payload = f[0], f[1], f[4:-4]
        if ftype == LOG:
            self.on_log(payload.decode(errors="replace"))
        elif ftype == EVENT and len(payload) >= 2:
            self.on_event(struct.unpack_from("<H", payload)[0], payload[2:])
        elif ftype in (OK, ERR):
            with self.lock:
                slot = self.pending.get(seq)
            if slot is not None:
                slot.append((ftype, payload))
                slot[0].set()

    def request(self, cmd, args=b"", timeout=2.0):
        with self.lock:
            self.seq = (self.seq + 1) & 0xFF
            seq = self.seq
            ev = threading.Event()
            self.pending[seq] = [ev]
        payload = struct.pack("<H", cmd) + args
        frame = struct.pack("<BBH", REQUEST, seq, len(payload)) + payload
        frame += struct.pack("<I", zlib.crc32(frame))
        self.ser.write(b"\x00" + cobs_encode(frame) + b"\x00")
        ok = ev.wait(timeout)
        with self.lock:
            slot = self.pending.pop(seq)
        if not ok:
            raise LinkError(f"no response to command {cmd:04x}")
        ftype, body = slot[1]
        if ftype == ERR:
            code = body[2] if len(body) > 2 else 0
            msg = body[3:].decode(errors="replace")
            raise LinkError(f"command {cmd:04x}: {ERRORS.get(code, code)}"
                            + (f": {msg}" if msg else ""))
        return body[2:]


class Reader:
    """Sequential decoder for response payloads."""

    def __init__(self, data):
        self.d, self.i = data, 0

    def take(self, fmt):
        v = struct.unpack_from(fmt, self.d, self.i)
        self.i += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    def u8(self):
        return self.take("<B")

    def u16(self):
        return self.take("<H")

    def u32(self):
        return self.take("<I")

    def u64(self):
        return self.take("<Q")

    def str(self):
        n = self.u8()
        s = self.d[self.i:self.i + n].decode(errors="replace")
        self.i += n
        return s


def wait_forever():
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
