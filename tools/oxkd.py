"""The console's kernel debugger (GDB RSP) on its COM port, bridge channel B.

The FPGA's COM UART runs at a fixed baud whatever the guest programs (01-interface UART_DIV).
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
    s = serial.Serial(None, baud, timeout=0)
    s.dtr = False
    s.rts = False
    s.port = port
    s.open()
    return s


def probe(com, wait):
    """Asks the stub why it stopped until it answers or `wait` seconds pass; returns the stop
    reply, or None. The leading '+' acks whatever it last sent, it ignores one it isn't
    waiting for."""
    com.reset_input_buffer()
    rx = b""
    end = time.time() + wait
    while time.time() < end:
        com.write(b"+" + rsp(b"?"))
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


def bridge(com, port, verbose=False):
    """TCP on 127.0.0.1 <-> the COM port, one debugger at a time. Console output with no
    debugger attached is dropped."""
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
        try:
            while True:
                if select.select([cli], [], [], 0.001)[0]:
                    data = cli.recv(4096)
                    if not data:
                        break
                    if verbose:
                        print(f"> {data.decode(errors='replace')}", flush=True)
                    com.write(data)
                data = com.read(4096)
                if data:
                    if verbose:
                        print(f"< {data.decode(errors='replace')}", flush=True)
                    cli.sendall(data)
        except OSError:
            pass
        cli.close()
        print("Debugger disconnected", flush=True)
