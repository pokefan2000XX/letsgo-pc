#!/usr/bin/env python3
"""Smoke test for the emulator harness: run an original game function and
check its internal results against an independent implementation.

0x7100101b30 compares two strings by Game Freak's FNV-1a 64 hash
(offset basis 0xCBF29CE484222645, prime 0x100000001B3): it hashes the string
at obj+0x88 and its second argument, then returns hash1 == hash2. The test
stops at the final compare to read both hashes, then runs to the return.

Usage: python -P tests/smoke/emu_fnv.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "emu"))
from guest_emu import GuestEmu  # noqa: E402
from unicorn.arm64_const import UC_ARM64_REG_X8, UC_ARM64_REG_X10  # noqa: E402

FUNC = 0x7100101B30
CMP = 0x7100101B94


def gf_fnv1a64(s):
    h = 0xCBF29CE484222645
    for c in s.encode():
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    emu = GuestEmu(os.path.join(ROOT, "orig", "exefs", "main"))
    ok = True
    for a, b in (("bin/archive/field/area01.gfpak", "bin/archive/field/area01.gfpak"),
                 ("pm0025_00", "pm0133_00"), ("", "x")):
        obj = emu.alloc(0x100)
        emu.uc.mem_write(obj + 0x88, struct.pack("<Q", emu.cstr(a)))
        sb = emu.cstr(b)
        emu.call(FUNC, obj, sb, until=CMP)
        h1, h2 = emu.uc.reg_read(UC_ARM64_REG_X10), emu.uc.reg_read(UC_ARM64_REG_X8)
        eq = emu.call(FUNC, obj, sb)
        good = h1 == gf_fnv1a64(a) and h2 == gf_fnv1a64(b) and eq == int(a == b)
        ok &= good
        print("%-4s %-32r %016x %-12r %016x -> %d" % ("ok" if good else "BAD", a, h1, b, h2, eq))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
