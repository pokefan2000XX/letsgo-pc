#!/usr/bin/env python3
"""Build the decomp patch module and recompile it with the game.

  python -P decomp/tools/build.py [--ninja PATH] [--hack NAME]... [--out DIR] [--gen DIR]

1. Compiles decomp/src for the Switch's CPU (decomp/CMakeLists.txt, cmake/guest-aarch64.cmake)
   into build/decomp/libdecomp.a.
2. Resolves every symbol the code uses but doesn't define to the original's address:
   decomp/symbols.txt, then FUN_/DAT_<address> names, then main's imports (calls go through
   main's own PLT stubs, so they reach the SDK the way main's calls do), then main's and the
   sdk's exported symbols. Anything left is an error.
   ORIG_<address> is the original function itself, for hooks: a call to it runs the original even
   when the decomp replaces it (recomp/recomp.py's ORIGINALS range), so a replacement can call the
   function it replaces.
   symbols.txt names a symbol by its C++ name without parameters (pokemon::Accessor::IsBadEgg),
   by its full signature when overloads share a name (pokemon::Accessor::Serialize(void*)), or
   mangled. A destructor's name covers the complete-object and base destructors; the deleting
   one is "Class::~Class [deleting]". A thunk is "non-virtual thunk to Class::~Class" (and so on).
3. Links build/decomp/patch.elf at 0x7102000000 (decomp/patch.ld). A vtable the code defines and
   symbols.txt names ("vtable for pokemon::Pokemon") is checked against the original's entry by
   entry (each virtual function must be the original or replace it), then the original is used,
   so objects look the same as the original's.
4. Every function it defines under a name symbols.txt gives an address (or FUN_<address>)
   replaces that original: build/decomp/replaces.json.
5. Recompiles it (recomp/recomp.py patch) into build/gen/patch, with a stub for each
   replaced function. main's recompiled code needs no change: the stubs override it at link time.

Needs build/gen/main (python -P recomp/recomp.py main sdk). The main build (CMakeLists.txt)
runs this whenever decomp/ changes.

--hack NAME also builds hacks/NAME/code into the module, as decompiled code is (new code, and
hooks that replace a function and call its original through ORIG_<address>).

--out DIR builds it in DIR/decomp and DIR/gen/patch instead (main is still read from build/gen, or
--gen), so two builds with different hacks don't overwrite one module (CMakeLists.txt's
DECOMP_OUT_DIR).
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
PATCH_OUT = os.path.join(ROOT, "build", "gen")     # the recompiled module goes in PATCH_OUT/patch
MAIN_OUT = os.path.join(ROOT, "build", "gen")      # where main was translated
DEFAULT_NINJA = "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
MAIN_BASE, SDK_BASE = 0x7100000000, 0x7180000000
ADDR_NAME = re.compile(r"^(?:FUN|DAT|PTR|LAB)_(71[0-9a-fA-F]{8})$")
ORIG_NAME = re.compile(r"^ORIG_(71[0-9a-fA-F]{8})$")
ORIGINALS = 0x7104000000                     # recomp/recomp.py: ORIG_<address> calls land here
VTABLE = "vtable for "
SPECIAL = (VTABLE, "VTT for ", "construction vtable for ", "typeinfo for ", "typeinfo name for ", "guard variable for ")
THUNKS = ("non-virtual thunk to ", "virtual thunk to ", "covariant return thunk to ")


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
    depth, i = 0, 0
    while i < len(dem):
        ch = dem[i]
        if dem.startswith("()", i) and dem.endswith("operator", 0, i):     # operator() is a name
            i += 2
            continue
        if ch in "<(":
            if ch == "(" and depth == 0 and i:
                dem = dem[:i]
                break
            depth += 1
        elif ch in ">)":
            depth -= 1
        i += 1
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
    """Every way symbols.txt may name this symbol. Vtables and the like go by their full name;
    a thunk is "non-virtual thunk to " and its function's name."""
    if dem.startswith(SPECIAL):
        return {mangled, dem}
    thunk = next((p for p in THUNKS if dem.startswith(p)), "")
    name = dem[len(thunk):]
    if "::~" in name and mangled.endswith("D0Ev"):     # deleting destructor
        return {mangled, dem + " [deleting]", thunk + strip_params(name) + " [deleting]"}
    return {mangled, dem, thunk + strip_params(name)}


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
    ap.add_argument("--hack", action="append", default=[], help="also build hacks/NAME/code into the module")
    ap.add_argument("--out", help="build the module in DIR/decomp and recompile it into DIR/gen/patch (default build/)")
    ap.add_argument("--gen", help="where main was translated (default build/gen)")
    a = ap.parse_args()
    global OUT, PATCH_OUT, MAIN_OUT
    if a.out:
        OUT, PATCH_OUT = os.path.join(a.out, "decomp"), os.path.join(a.out, "gen")
    if a.gen:
        MAIN_OUT = a.gen
    os.makedirs(OUT, exist_ok=True)
    extra = []
    for hack in a.hack:
        code = os.path.join(ROOT, "hacks", hack, "code")
        if not os.path.isdir(code):
            raise SystemExit("decomp: no hack code in %s" % code)
        extra.append(code.replace("\\", "/"))
    srcs = [os.path.join(d, f) for top in [os.path.join(ROOT, "decomp", "src")] + extra for d, _, fs in os.walk(top) for f in fs
            if f.endswith((".cpp", ".c"))]
    elf = os.path.join(OUT, "patch.elf")
    replaces_path = os.path.join(OUT, "replaces.json")
    recomp = [sys.executable, "-P", os.path.join(ROOT, "recomp", "recomp.py"), "patch",
              "--out", PATCH_OUT, "--main-out", MAIN_OUT]
    if not srcs:                                     # nothing decompiled: an empty patch module
        for p in (elf, replaces_path):
            if os.path.exists(p):
                os.remove(p)
        print(run(recomp).strip())
        touch_outputs()
        return

    # 1. compile
    cfg = os.path.join(OUT, "cmake")
    # configured every time: the hacks built in can change between builds
    run(["cmake", "-S", os.path.join(ROOT, "decomp"), "-B", cfg, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + a.ninja,
         "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ROOT, "cmake", "guest-aarch64.cmake").replace("\\", "/"),
         "-DCMAKE_BUILD_TYPE=Release", "-DDECOMP_EXTRA_DIRS=" + ";".join(extra)])
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
    starts = None
    for name in sorted(undefined):
        m = ADDR_NAME.match(name)
        o = ORIG_NAME.match(name)
        if name in names:
            resolved[name] = names[name]
        elif m:
            resolved[name] = int(m.group(1), 16)
        elif o:
            if starts is None:
                starts = {int(r["Address"], 16) for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_functions.csv")))}
            addr = int(o.group(1), 16)
            if addr not in starts:
                raise SystemExit("decomp: %s: 0x%x isn't the start of one of main's functions" % (name, addr))
            resolved[name] = ORIGINALS + addr - MAIN_BASE
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
    link = [os.path.join(BIN, "ld.lld.exe"), "-T", os.path.join(ROOT, "decomp", "patch.ld"), os.path.join(OUT, "symbols.ld"),
            "--whole-archive", lib, "--no-whole-archive", "-static", "--no-pie", "-e", "0", "-o", elf]
    run(link)
    vtables = {m: names[m] for m in defined if dem.get(m, m).startswith(VTABLE) and m in names}
    if vtables:
        check_vtables(elf, vtables, names, resolved, dem, defined)
        with open(os.path.join(OUT, "symbols.ld"), "a", newline="\n") as f:
            f.write("/* vtables checked against the original's, used in their place */\n")
            for name, addr in sorted(vtables.items()):
                f.write('"%s" = 0x%x;\n' % (name, addr))
                resolved[name] = addr
        run(link)
    syms = elf_symbols_abs(elf)
    end = max((syms[n] for n in defined if n in syms), default=0)
    if end >= ORIGINALS:
        raise SystemExit("decomp: the patch module reaches 0x%x, past 0x%x where ORIG_ calls go" % (end, ORIGINALS))
    if any(sec == ".init_array" and size for sec, size in elf_sections(elf)):
        raise SystemExit("decomp: global objects with constructors aren't supported in decompiled code yet (.init_array)")
    check_no_jump_tables(elf)

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


def check_no_jump_tables(elf):
    """The recompiler only follows indirect branches to function entries. A jump table (an offset
    loaded from a table next to an `adr`, added to a base, then `br`) branches into a function, so
    the game would stop there: decomp/CMakeLists.txt compiles with -fno-jump-tables to avoid them."""
    out = run([os.path.join(BIN, "llvm-objdump.exe"), "-d", "--no-show-raw-insn", elf])
    recent = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if not m:
            recent = []
            continue
        addr, mn, ops = m.groups()
        if mn == "br" and any(p[0] in ("ldrb", "ldrh", "ldrsw") for p in recent) and \
                any(p[0] == "add" and "lsl #2" in p[1] for p in recent):
            raise SystemExit("decomp: a jump table at 0x%s (br %s): build decomp/ with -fno-jump-tables" % (addr, ops))
        recent = (recent + [(mn, ops)])[-4:]


def check_vtables(elf, vtables, names, resolved, dem, defined):
    """Each vtable the code defines must match the original's: the same offsets, and each virtual
    function the original one or a replacement for it."""
    symtab = elf_symbol_table(elf)
    by_addr = {}                                    # our functions by address
    for name, (value, _size) in symtab.items():
        if defined.get(name, "") in ("T", "t", "W", "w"):
            by_addr.setdefault(value, []).append(name)
    pure = resolved.get("__cxa_pure_virtual")
    errors = []
    for name, orig in sorted(vtables.items()):
        value, size = symtab[name]
        ours = struct.unpack("<%dQ" % (size // 8), elf_read(elf, value, size))
        theirs = original_words(orig, len(ours))
        for k, (w, o) in enumerate(zip(ours, theirs)):
            if isinstance(o, tuple):                    # an import: the original has its relocation
                if (o[1] == "__cxa_pure_virtual" and w == pure) or w == resolved.get(o[1]):
                    continue
                errors.append("%s[%d]: 0x%x, the original imports %s" % (dem.get(name, name), k, w, o[1]))
                continue
            if w in by_addr:                                # one of ours: what it replaces
                targets = {names.get(n) for n in by_addr[w]} | {
                    int(m.group(1), 16) for m in map(ADDR_NAME.match, by_addr[w]) if m}
                targets.discard(None)
                if o in targets:
                    continue
                what = ", ".join(dem.get(n, n) for n in by_addr[w])
                errors.append("%s[%d]: %s replaces %s, the original has 0x%x" % (
                    dem.get(name, name), k, what, ", ".join("0x%x" % t for t in targets) or "nothing", o))
            elif w != o:
                errors.append("%s[%d]: 0x%x, the original has 0x%x" % (dem.get(name, name), k, w, o))
    if errors:
        raise SystemExit("decomp: vtables differ from the original's:\n  " + "\n  ".join(errors))


_main_nso = []


def original_words(addr, count):
    """Words of main at an address, as loaded: relocated pointers, ("import", name) for imports."""
    if not _main_nso:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import nso2elf
        nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
        info = nso2elf.analyze(nso)
        _main_nso.extend([nso["image"], {r["offset"]: r for r in info["relocs"]}, info["symbols"]])
    image, relocs, symbols = _main_nso
    out = []
    for k in range(count):
        off = addr - MAIN_BASE + 8 * k
        r = relocs.get(off)
        if r is not None and r["type"] == 1027:        # R_AARCH64_RELATIVE
            out.append(MAIN_BASE + r["addend"])
        elif r is not None and r["sym"]:
            out.append(("import", symbols[r["sym"]]["name"]))
        else:
            out.append(struct.unpack_from("<Q", image, off)[0])
    return out


def elf_symbol_table(path):
    """Symbols of the linked patch module: name -> (address, size)."""
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
            st_name, _info, _other, shndx, value, sz = struct.unpack_from("<IBBHQQ", data, off + k * entsize)
            if shndx and st_name:
                end = data.index(b"\0", stroff + st_name)
                out[data[stroff + st_name:end].decode("latin1")] = (value, sz)
    return out


def elf_read(path, addr, size):
    """Bytes of the linked patch module at an address."""
    data = open(path, "rb").read()
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    for i in range(shnum):
        _n, typ, _fl, sh_addr, off, sh_size, *_ = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        if typ != 8 and sh_addr <= addr and addr + size <= sh_addr + sh_size:   # not NOBITS
            return data[off + addr - sh_addr:off + addr - sh_addr + size]
    raise SystemExit("decomp: 0x%x isn't in the patch module's data" % addr)


def touch_outputs():
    """recomp.py leaves unchanged files alone; the build still needs them newer than decomp/."""
    for f in ("f000.c", "table.c"):
        p = os.path.join(PATCH_OUT, "patch", f)
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
