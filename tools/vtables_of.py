"""Print the slots of named C++ vtables in an NSO, resolved to symbol names.

Usage: python -P tools/vtables_of.py orig/exefs/subsdk1 movie::Extractor movie::Decoder
Each argument is a class name; its vtable symbol (_ZTV...) is looked up in the
module's dynamic symbols, and each 8-byte slot is resolved through the module's
relocations (RELATIVE -> the defined symbol at that address, ABS64 -> the named
import). Slot offsets are relative to the address point (vtable + 16), which is
what the code's `ldr x8, [x0]; ldr x9, [x8, #off]` sequences use.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso2elf  # noqa: E402

LLVM = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "toolchain", "llvm-mingw", "bin")


def mangle_vtable(cls):
    parts = cls.split("::")
    return "_ZTVN" + "".join("%d%s" % (len(p), p) for p in parts) + "E" if len(parts) > 1 else \
        "_ZTV%d%s" % (len(cls), cls)


def demangle(names):
    exe = os.path.join(LLVM, "llvm-cxxfilt.exe")
    if not names or not os.path.exists(exe):
        return names
    out = subprocess.run([exe], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return out if len(out) == len(names) else names


def main():
    nso = nso2elf.load_nso(sys.argv[1])
    info = nso2elf.analyze(nso)
    syms = info["symbols"]
    by_addr = {}
    for s in syms:
        if s["shndx"] and s["value"] and s["type"] in (1, 2):
            by_addr.setdefault(s["value"], s["name"])
    rel_at = {r["offset"]: r for r in info["relocs"]}
    img = nso["image"]
    for cls in sys.argv[2:]:
        vt = mangle_vtable(cls)
        sym = next((s for s in syms if s["name"] == vt and s["shndx"]), None)
        if not sym:
            print("%s: no vtable symbol %s" % (cls, vt))
            continue
        print("%s (vtable at 0x%x, %d bytes)" % (cls, sym["value"], sym["size"]))
        slots = []
        for off in range(16, sym["size"], 8):
            a = sym["value"] + off
            r = rel_at.get(a)
            if r and r["type"] == 1027:                   # R_AARCH64_RELATIVE
                name = by_addr.get(r["addend"], "0x%x" % r["addend"])
            elif r and r["type"] in (257, 1025):          # ABS64 / GLOB_DAT
                name = syms[r["sym"]]["name"] or "?"
            else:
                name = "0x%x" % nso2elf.u64(img, a)
            slots.append((off - 16, name))
        for (off, _), dem in zip(slots, demangle([n for _, n in slots])):
            print("  +0x%02x  %s" % (off, dem))


if __name__ == "__main__":
    main()
