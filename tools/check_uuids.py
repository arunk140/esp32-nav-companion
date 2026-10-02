#!/usr/bin/env python3
"""Read-only checker: every *_UUID string must equal its *_UUID_LE byte array.

NimBLE stores 128-bit UUIDs little-endian (string bytes reversed). This script
only reads and reports; it never rewrites the contract header (see guide 6.2/6.3).
"""
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "firmware_v2/main/ble_service.h"


def parse(path):
    text = open(path).read()
    # Continuation-aware: collapse backslash-newline.
    text = re.sub(r"\\\s*\n", " ", text)
    strings = {}
    arrays = {}
    for m in re.finditer(r"#define\s+(\w+?_UUID)\s+\"([0-9a-fA-F-]+)\"", text):
        strings[m.group(1)] = m.group(2)
    for m in re.finditer(r"#define\s+(\w+?_UUID_LE)\s+((?:0x[0-9a-fA-F]{1,2}\s*,?\s*)+)", text):
        nums = re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2))
        arrays[m.group(1)] = [int(n, 16) for n in nums]
    return strings, arrays


def main():
    strings, arrays = parse(SRC)
    ok = True
    for name, s in sorted(strings.items()):
        le_name = name + "_LE"
        arr = arrays.get(le_name)
        if arr is None:
            print(f"MISSING  {le_name} for {name}")
            ok = False
            continue
        expect = bytes.fromhex(s.replace("-", ""))[::-1]
        if len(arr) != 16:
            print(f"LEN      {le_name}: {len(arr)} bytes, expected 16")
            ok = False
        if list(expect) != arr:
            print(f"MISMATCH {name}")
            print(f"  string: {s}")
            print(f"  expect: {' '.join(f'{b:02x}' for b in expect)}")
            print(f"  actual: {' '.join(f'{b:02x}' for b in arr)}")
            ok = False
        else:
            print(f"ok       {name}")
    # The service UUID's string lives in the app (BleManager.kt), not here, so a
    # missing string twin is informational rather than a failure.
    for name in arrays:
        if name[:-3] not in strings:
            print(f"note     {name}: no matching _UUID string in this header")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
