#!/usr/bin/env python3
"""Convert a binary file to $readmemh format (one byte per line).

    py -3 tools/bin2hex.py <in.bin> <out.hex>
"""
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("\n".join(f"{b:02x}" for b in data) + "\n")
print(f"{len(data)} bytes -> {sys.argv[2]}")
