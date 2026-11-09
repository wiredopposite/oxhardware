#!/usr/bin/env python3
"""Toggle the lpc_serve 'force gate' bench test and show the new state.

    py -3 tools/gate.py COM11
LED goes red while the gate is forced on. Only use with the Xbox powered off.
"""
import re
import sys
import time

import serial

if len(sys.argv) != 2:
    sys.exit(__doc__)
s = serial.Serial(sys.argv[1], 115200, timeout=0.3)
s.write(b"g")
time.sleep(0.1)
s.reset_input_buffer()
s.write(b"\r")
t, buf = time.time(), b""
while time.time() - t < 2.5:
    buf += s.read(4096)
s.close()
w = {int(a, 16): int(b, 16) for a, b in re.findall(rb"([0-9A-F]{2}) ([0-9A-F]{8})", buf)}
print("gate forced ON (measure now)" if w.get(0x24, 0) & 2 else "gate released (normal)")
