#!/usr/bin/env python3
"""Check that every NVN type in nnsdk's nvn.h has the same size and
alignment on the host (x86-64 Windows, llvm-mingw) as on the guest (AArch64).
The game allocates NVN objects in its own memory and the runtime's NVN-on-Vulkan
layer works on them in place, so the layouts must agree.

Usage: python -P tests/smoke/nvn_layout.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "toolchain", "llvm-mingw", "bin")
INC = os.path.join(ROOT, "toolchain", "nnsdk", "lib", "nvn", "include")
OUT = os.path.join(ROOT, "build", "smoke")


def main():
    os.makedirs(OUT, exist_ok=True)
    src = open(os.path.join(INC, "nvn", "nvn.h"), encoding="utf-8").read()
    names = sorted(set(re.findall(r"\}\s*(NVN[A-Za-z0-9_]+)\s*;", src)))
    host_c = os.path.join(OUT, "nvn_sizes.c")
    with open(host_c, "w") as f:
        f.write('#include <stdio.h>\n#include "nvn/nvn.h"\nint main(void){\n')
        for n in names:
            f.write('printf("%%s %%zu %%zu\\n", "%s", sizeof(%s), _Alignof(%s));\n' % (n, n, n))
        f.write("return 0;}\n")
    exe = os.path.join(OUT, "nvn_sizes.exe")
    subprocess.run([os.path.join(BIN, "clang.exe"), "-std=c11", "-static", "-I", INC, host_c, "-o", exe], check=True)
    rows = [l.split() for l in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines()]
    guest_c = os.path.join(OUT, "nvn_sizes_guest.c")
    with open(guest_c, "w") as f:
        f.write('#include "nvn/nvn.h"\n')
        for n, s, a in rows:
            f.write('_Static_assert(sizeof(%s) == %s && _Alignof(%s) == %s, "%s differs");\n' % (n, s, n, a, n))
    r = subprocess.run([os.path.join(BIN, "clang.exe"), "--target=aarch64-none-elf", "-ffreestanding", "-std=c11",
                        "-fsyntax-only", "-I", INC, guest_c], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
        print("FAIL")
        return 1
    print("PASS: %d NVN types have identical layouts on host and guest" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
