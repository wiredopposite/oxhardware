"""The console's kernel debugger (GDB RSP) on its COM port, bridge channel B.

The FPGA's COM UART runs at a fixed baud whatever the console programs.
"""
import re
import select
import socket
import time

import serial

STOP = re.compile(rb"\$([TS][^#]*)#")

def rsp(body):
    return b"$" + body + b"#%02x" % (sum(body) & 0xFF)

def open_com(port, baud):
    # DTR stays asserted: a USB UART bridge like the Debug Probe only forwards what it receives
    # while the host asserts it. A write that can't drain mustn't hang the probe.
    return serial.Serial(port, baud, timeout=0, write_timeout=0.5)

def probe(com, wait):
    """Asks the stub why it stopped until it answers or `wait` seconds pass; returns the stop
    reply, or None. The leading '+' acks whatever it last sent, it ignores one it isn't
    waiting for."""
    com.reset_input_buffer()
    rx = b""
    end = time.time() + wait
    while time.time() < end:
        try:
            com.write(b"+" + rsp(b"?"))
        except serial.SerialTimeoutException:
            pass
        until = time.time() + 0.5
        while time.time() < until:
            rx += com.read(4096)
            m = STOP.search(rx)
            if m:
                return m.group(1).decode(errors="replace")
            time.sleep(0.01)
    return None

def kill(com):
    """Stopped in the debugger, the stub resets the console through the SMC."""
    com.write(b"+" + rsp(b"k"))
    com.flush()

FRAME = re.compile(rb"\$([^#$]*)#[0-9a-fA-F]{2}")
# What the kernel says just before it resets the console itself
RESET_NOTICE = "OpenOX: resetting"

class Packets:
    """Whole packet bodies out of a byte stream, as they arrive."""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        bodies = []
        while True:
            m = FRAME.search(self.buf)
            if not m:
                # Keep from the last '$': a packet still arriving
                start = self.buf.rfind(b"$")
                self.buf = self.buf[start:] if start >= 0 else b""
                return bodies
            bodies.append(m.group(1))
            self.buf = self.buf[m.end():]

class Session:
    """What the debugger set up on the stub, put back after the kernel resets the console:
    a new boot starts a new stub, but to the debugger it's the same process."""

    def __init__(self):
        self.noack = False
        self.breaks = {}        # (type, address) -> the Z packet

    def from_host(self, body):
        if body == b"QStartNoAckMode":
            self.noack = True
        elif body[:1] in (b"Z", b"z"):
            key = tuple(body[1:].split(b",")[:2])
            if body[:1] == b"Z":
                self.breaks[key] = body
            else:
                self.breaks.pop(key, None)
        elif body[:1] in (b"D", b"k"):
            self.noack = False
            self.breaks.clear()

def resetting(body):
    if body[:1] != b"O" or body == b"OK":
        return False
    try:
        return RESET_NOTICE in bytes.fromhex(body[1:].decode()).decode(errors="replace")
    except ValueError:
        return False

def request(com, body, noack, wait=2.0):
    """Sends a packet to the stub and returns its reply body, or None."""
    com.write(rsp(body))
    rx = Packets()
    end = time.time() + wait
    while time.time() < end:
        for reply in rx.feed(com.read(4096)):
            if not noack:
                com.write(b"+")
            if reply[:1] != b"O":
                return reply
        time.sleep(0.005)
    return None

def carry(com, cli, session, verbose):
    """The console is resetting: waits for the next boot's boot break, sets the stub up again
    and gives the debugger that stop as the answer to the continue it's waiting on. A pause
    asked for meanwhile is answered by the same stop."""
    print("Console resetting, carrying the session over", flush=True)
    time.sleep(0.5)
    com.reset_input_buffer()
    stop = None
    end = time.time() + 60
    while stop is None and time.time() < end:
        if select.select([cli], [], [], 0)[0] and not cli.recv(4096):
            return False
        stop = probe(com, 1)
    if stop is None:
        print("The console didn't come back to its boot break", flush=True)
        return True
    if session.noack and request(com, b"QStartNoAckMode", False) != b"OK":
        print("No-ack mode not taken up again", flush=True)
    for body in session.breaks.values():
        if request(com, body, session.noack) != b"OK":
            print(f"Breakpoint not put back: {body.decode(errors='replace')}", flush=True)
    if verbose:
        print(f"< {stop}", flush=True)
    cli.sendall(rsp(stop.encode()))
    print(f"Stopped at the next boot's boot break, {len(session.breaks)} breakpoints put back",
          flush=True)
    return True

def bridge(com, port, verbose=False):
    """TCP on 127.0.0.1 <-> the COM port, one debugger at a time. Console output with no
    debugger attached is dropped. When the kernel resets the console itself, the debugger
    stays connected and stops at the next boot's boot break."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"Listening on 127.0.0.1:{port}, in lldb: gdb-remote {port}", flush=True)
    while True:
        if not select.select([srv], [], [], 0.01)[0]:
            com.read(4096)
            continue
        cli, _ = srv.accept()
        cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("Debugger connected", flush=True)
        session = Session()
        to_stub, to_host = Packets(), Packets()
        try:
            while True:
                if select.select([cli], [], [], 0.001)[0]:
                    data = cli.recv(4096)
                    if not data:
                        break
                    if verbose:
                        print(f"> {data.decode(errors='replace')}", flush=True)
                    for body in to_stub.feed(data):
                        session.from_host(body)
                    com.write(data)
                data = com.read(4096)
                if data:
                    if verbose:
                        print(f"< {data.decode(errors='replace')}", flush=True)
                    cli.sendall(data)
                    if any(resetting(body) for body in to_host.feed(data)):
                        if not carry(com, cli, session, verbose):
                            break
                        to_stub, to_host = Packets(), Packets()
        except OSError:
            pass
        cli.close()
        print("Debugger disconnected", flush=True)
