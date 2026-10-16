#!/usr/bin/env python3
"""Build the decomp patch module and recompile it with the game.

  python -P decomp/tools/build.py [--ninja PATH]

1. Compiles decomp/src for the Switch's CPU (decomp/CMakeLists.txt, cmake/guest-aarch64.cmake)
   into build/decomp/libdecomp.a.
2. Resolves every symbol the code uses but doesn't define to the original's address:
   decomp/symbols.txt, then FUN_/DAT_<address> names, then main's imports (calls go through
   main's own PLT stubs, so they reach the SDK the way main's calls do), then main's and the
   sdk's exported symbols. Anything left is an error.
   symbols.txt names a symbol by its C++ name without parameters (pokemon::Accessor::IsBadEgg),
   by its full signature when overloads share a name (pokemon::Accessor::Serialize(void*)), or
   mangled. A destructor's name covers the complete-object and base destructors; the deleting
   one is "Class::~Class [deleting]".
3. Links build/decomp/patch.elf at 0x7102000000 (decomp/patch.ld).
4. Every function it defines under a name symbols.txt gives an address (or FUN_<address>)
   replaces that original: build/decomp/replaces.json.
5. Recompiles it (recomp/recomp.py patch) into build/gen/patch, with a stub for each
   replaced function. main's recompiled code needs no change: the stubs override it at link time.

Needs build/gen/main (python -P recomp/recomp.py main sdk). The main build (CMakeLists.txt)
runs this whenever decomp/ changes.
"""
import argparse
import csv
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "toolchain", "llvm-mingw", "bin")
OUT = os.path.join(ROOT, "build", "decomp")
DEFAULT_NINJA = "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
MAIN_BASE, SDK_BASE = 0x7100000000, 0x7180000000
ADDR_NAME = re.compile(r"^(?:FUN|DAT|PTR|LAB)_(71[0-9a-fA-F]{8})$")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("decomp: %s failed" % os.path.basename(cmd[0]))
    return r.stdout


def elf_symbols(path, base):
    """Defined symbols of one of the game's ELFs (orig/elf, linked at 0): name -> guest address."""
    data = open(path, "rb").read()
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for _name, typ, _fl, _addr, off, size, link, _info, _al, entsize in secs:
        if typ not in (2, 11) or not entsize:           # SHT_SYMTAB, SHT_DYNSYM
            continue
        stroff = secs[link][4]
        for k in range(size // entsize):
            st_name, _info, _other, shndx, value, _sz = struct.unpack_from("<IBBHQQ", data, off + k * entsize)
            if shndx and st_name:
                end = data.index(b"\0", stroff + st_name)
                out.setdefault(data[stroff + st_name:end].decode("latin1"), base + value)
    return out


def strip_params(dem):
    """'unsigned short pokemon::Foo<int>(int) const' -> 'pokemon::Foo<int>'."""
    depth = 0
    for i, ch in enumerate(dem):
        if ch in "<(":
            if ch == "(" and depth == 0 and i:
                dem = dem[:i]
                break
            depth += 1
        elif ch in ">)":
            depth -= 1
    depth, start = 0, 0
    for i, ch in enumerate(dem):             # drop a return type (template functions print one)
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == " " and depth == 0:
            start = i + 1
    return dem[start:]


def name_keys(mangled, dem):
    """Every way symbols.txt may name this symbol."""
    if "::~" in dem and mangled.endswith("D0Ev"):      # deleting destructor
        return {mangled, dem + " [deleting]", strip_params(dem) + " [deleting]"}
    return {mangled, dem, strip_params(dem)}


def read_symbols_txt():
    names = {}
    for n, line in enumerate(open(os.path.join(ROOT, "decomp", "symbols.txt"), encoding="utf-8"), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            raise SystemExit("decomp/symbols.txt:%d: expected '<address> <name>  # notes'" % n)
        name = parts[1].strip()
        if name in names:
            raise SystemExit("decomp/symbols.txt:%d: %s is listed twice" % (n, name))
        names[name] = int(parts[0], 16)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ninja", default=os.environ.get("NINJA", DEFAULT_NINJA))
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    srcs = [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(ROOT, "decomp", "src")) for f in fs
            if f.endswith((".cpp", ".c"))]
    elf = os.path.join(OUT, "patch.elf")
    replaces_path = os.path.join(OUT, "replaces.json")
    recomp = [sys.executable, "-P", os.path.join(ROOT, "recomp", "recomp.py"), "patch",
              "--out", os.path.join(ROOT, "build", "gen")]
    if not srcs:                                     # nothing decompiled: an empty patch module
        for p in (elf, replaces_path):
            if os.path.exists(p):
                os.remove(p)
        print(run(recomp).strip())
        touch_outputs()
        return

    # 1. compile
    cfg = os.path.join(OUT, "cmake")
    if not os.path.exists(os.path.join(cfg, "CMakeCache.txt")):
        run(["cmake", "-S", os.path.join(ROOT, "decomp"), "-B", cfg, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + a.ninja,
             "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ROOT, "cmake", "guest-aarch64.cmake").replace("\\", "/"),
             "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", cfg])
    lib = os.path.join(cfg, "libdecomp.a")

    # 2. resolve
    nm = os.path.join(BIN, "llvm-nm.exe")
    defined, undefined = {}, set()
    for line in run([nm, "-g", "-P", lib]).splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0].endswith(":") or line.endswith(":"):
            continue
        name, kind = parts[0], parts[1]
        if kind in "Uw":
            undefined.add(name)
        else:
            defined[name] = kind
    undefined -= set(defined)
    every = sorted(set(defined) | undefined)
    dem = dict(zip(every, run([os.path.join(BIN, "llvm-cxxfilt.exe")], input="\n".join(every) + "\n").splitlines()))
    by_key = {}
    for m in every:
        for k in name_keys(m, dem.get(m, m)):
            by_key.setdefault(k, set()).add(m)
    names = {}                                   # mangled name -> original address
    for entry, addr in read_symbols_txt().items():
        for m in by_key.get(entry, ()):
            if m in names and names[m] != addr:
                raise SystemExit("decomp/symbols.txt: %s gets two addresses (0x%x, 0x%x)" % (dem.get(m, m), names[m], addr))
            names[m] = addr
    imports = {}
    for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_imports.csv"))):
        if r["Kind"] == "func" and r["PLT"]:
            imports[r["Name"]] = int(r["PLT"], 16)
    exported = elf_symbols(os.path.join(ROOT, "orig", "elf", "sdk.elf"), SDK_BASE)
    exported.update(elf_symbols(os.path.join(ROOT, "orig", "elf", "main.elf"), MAIN_BASE))
    resolved, missing = {}, []
    for name in sorted(undefined):
        m = ADDR_NAME.match(name)
        if name in names:
            resolved[name] = names[name]
        elif m:
            resolved[name] = int(m.group(1), 16)
        elif name in imports:
            resolved[name] = imports[name]
        elif name in exported and exported[name] < SDK_BASE:
            resolved[name] = exported[name]
        elif name in exported:
            # data the sdk defines (main's functions reach the sdk through imports, never directly)
            resolved[name] = exported[name]
        else:
            missing.append(name)
    if missing:
        raise SystemExit("decomp: no address for %s\n  add them to decomp/symbols.txt (or name them FUN_<address>)" %
                         ", ".join(missing))
    with open(os.path.join(OUT, "symbols.ld"), "w", newline="\n") as f:
        f.write("/* original code and data the decomp module uses (decomp/tools/build.py) */\n")
        for name, addr in sorted(resolved.items()):
            f.write('"%s" = 0x%x;\n' % (name, addr))

    # 3. link
    run([os.path.join(BIN, "ld.lld.exe"), "-T", os.path.join(ROOT, "decomp", "patch.ld"), os.path.join(OUT, "symbols.ld"),
         "--whole-archive", lib, "--no-whole-archive", "-static", "--no-pie", "-e", "0", "-o", elf])
    syms = elf_symbols_abs(elf)
    if any(sec == ".init_array" and size for sec, size in elf_sections(elf)):
        raise SystemExit("decomp: global objects with constructors aren't supported in decompiled code yet (.init_array)")

    # 4. replacements
    replaces, by_orig = {}, {}
    for name in defined:
        orig = names.get(name)
        m = ADDR_NAME.match(name)
        if orig is None and m:
            orig = int(m.group(1), 16)
        if orig is None or defined[name] not in "TtWw":   # functions only
            continue
        if not MAIN_BASE <= orig < SDK_BASE:
            raise SystemExit("decomp: %s replaces 0x%x, which isn't in main" % (dem.get(name, name), orig))
        if "0x%x" % orig in replaces and replaces["0x%x" % orig] != "0x%x" % syms[name]:
            raise SystemExit("decomp: %s and %s both replace 0x%x (give the full signature in decomp/symbols.txt)" % (
                dem.get(by_orig[orig], by_orig[orig]), dem.get(name, name), orig))
        replaces["0x%x" % orig] = "0x%x" % syms[name]
        by_orig[orig] = name
    json.dump(replaces, open(replaces_path, "w"), indent=1, sort_keys=True)
    # name -> address of everything resolved, for the emulator (decomp/tools/difftest.py)
    json.dump({"resolved": {n: "0x%x" % v for n, v in resolved.items()},
               "defined": {n: "0x%x" % syms[n] for n in defined if n in syms},
               "demangled": {n: dem[n] for n in defined if n in syms and dem.get(n, n) != n}},
              open(os.path.join(OUT, "symbols.json"), "w"), indent=1, sort_keys=True)

    # 5. recompile
    print(run(recomp + ["--patch-elf", elf, "--replaces", replaces_path]).strip())
    touch_outputs()
    print("decomp: %d source files, %d replaced functions" % (len(srcs), len(replaces)))


def touch_outputs():
    """recomp.py leaves unchanged files alone; the build still needs them newer than decomp/."""
    for f in ("f000.c", "table.c"):
        p = os.path.join(ROOT, "build", "gen", "patch", f)
        if os.path.exists(p):
            os.utime(p)


def elf_symbols_abs(path):
    """Symbols of the linked patch module (absolute addresses)."""
    data = open(path, "rb").read()
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for _name, typ, _fl, _addr, off, size, link, _info, _al, entsize in secs:
        if typ != 2 or not entsize:
            continue
        stroff = secs[link][4]
        for k in range(size // entsize):
            st_name, _info, _other, shndx, value, _sz = struct.unpack_from("<IBBHQQ", data, off + k * entsize)
            if shndx and st_name:
                end = data.index(b"\0", stroff + st_name)
                out[data[stroff + st_name:end].decode("latin1")] = value
    return out


def elf_sections(path):
    data = open(path, "rb").read()
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    for name, _t, _fl, _addr, _off, size, _l, _i, _al, _es in secs:
        end = data.index(b"\0", stroff + name)
        yield data[stroff + name:end].decode("latin1"), size


if __name__ == "__main__":
    main()
