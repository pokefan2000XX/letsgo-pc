#!/usr/bin/env python3
"""Run original game functions under an AArch64 emulator (Unicorn).

This is the ground truth for testing: a recompiled or decompiled function is
correct when it produces the same results as the original running here.

  emu = GuestEmu("orig/exefs/main")
  emu.import_handlers["strlen"] = lambda e: len(e.read_cstr(e.reg(0)))
  result = emu.call(0x7100101b30, obj_ptr, str_ptr)

Memory map (same as the PC runtime):
  0x7100000000  main (relocated, .bss zeroed)
  0x7200000000  import trampolines: one 16-byte slot per dynamic symbol; reaching one
                calls the Python handler for that import (or stops with an error)
  0x7104000000  the originals by another address, for the patch module's ORIG_<address> calls
                (recomp/recomp.py's ORIGINALS): reaching one runs the original
  0x7300000000  stack (1 MiB) and TLS block
  0x7400000000  scratch heap for test inputs (bump allocator)
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm64_const import (UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_Q0,
                                 UC_ARM64_REG_SP, UC_ARM64_REG_TPIDR_EL0, UC_ARM64_REG_X0)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nso2elf  # noqa: E402

MAIN_BASE = 0x7100000000
STUB_BASE = 0x7200000000
STACK_BASE, STACK_SIZE = 0x7300000000, 0x100000
TLS_BASE = STACK_BASE + STACK_SIZE
HEAP_BASE, HEAP_SIZE = 0x7400000000, 0x1000000
RETURN_MAGIC = STUB_BASE + 0xFFF0   # LR for calls made by the harness
ORIGINALS, ORIGINALS_SPAN = 0x7104000000, 0x2000000


def _memcpy(e):
    d, s, n = e.reg(0), e.reg(1), e.reg(2)
    if n:
        e.uc.mem_write(d, bytes(e.uc.mem_read(s, n)))
    return d


def _memset(e):
    d, c, n = e.reg(0), e.reg(1) & 0xFF, e.reg(2)
    if n:
        e.uc.mem_write(d, bytes([c]) * n)
    return d


def _memcmp(e):
    a, b = bytes(e.uc.mem_read(e.reg(0), e.reg(2))), bytes(e.uc.mem_read(e.reg(1), e.reg(2)))
    for x, y in zip(a, b):
        if x != y:
            return (x - y) & 0xFFFFFFFFFFFFFFFF
    return 0


def _strlen(e):
    a, n = e.reg(0), 0
    while e.uc.mem_read(a + n, 1)[0]:
        n += 1
    return n


# libc functions every test may reach (pure memory operations)
LIBC_HANDLERS = {"memcpy": _memcpy, "memmove": _memcpy, "memset": _memset, "memcmp": _memcmp, "strlen": _strlen}


class GuestEmu:
    def __init__(self, nso_path):
        nso = nso2elf.load_nso(nso_path)
        info = nso2elf.analyze(nso)
        size = (max(info["bss_end"], nso["data"][0] + nso["data"][1] + nso["bss_size"]) + 0xFFF) & ~0xFFF
        image = bytearray(size)
        image[:len(nso["image"])] = nso["image"]
        self.symbols = info["symbols"]
        for r in info["relocs"]:
            t, s = r["type"], r["sym"]
            if t == 1027:
                val = MAIN_BASE + r["addend"]
            elif t in (257, 1025, 1026):
                y = self.symbols[s]
                base = MAIN_BASE + y["value"] if y["shndx"] else STUB_BASE + s * 16
                val = base + (r["addend"] if t == 257 else 0)
            else:
                raise ValueError("relocation type %d" % t)
            struct.pack_into("<Q", image, r["offset"], val & 0xFFFFFFFFFFFFFFFF)
        n_stub = (len(self.symbols) * 16 + 0x10000 + 0xFFF) & ~0xFFF
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(MAIN_BASE, size, UC_PROT_ALL)
        uc.mem_write(MAIN_BASE, bytes(image))
        uc.mem_map(STUB_BASE, n_stub, UC_PROT_ALL)
        uc.mem_write(STUB_BASE, b"\x00\x00\x20\xd4" * (n_stub // 4))   # brk #0 everywhere
        uc.mem_map(STACK_BASE, STACK_SIZE + 0x10000, UC_PROT_ALL)
        uc.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)   # enable FP/SIMD
        uc.reg_write(UC_ARM64_REG_TPIDR_EL0, TLS_BASE)
        self.heap_top = HEAP_BASE
        self.import_handlers = dict(LIBC_HANDLERS)
        self.host_funcs = {}             # stub index -> (name, handler): fake functions for tests
        self.n_stub_slots = n_stub // 16
        self.calls = []
        self.replacements = {}           # original -> its replacement in the patch module
        uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUB_BASE, end=STUB_BASE + n_stub - 1)

    def load_patch(self, elf_path):
        """Maps the decomp patch module (decomp/tools/build.py: a static ELF linked at its guest
        address, 0x7102000000) next to main, so decompiled functions run here too. Its calls into
        the SDK go through main's PLT stubs, so they reach the same import handlers."""
        data = open(elf_path, "rb").read()
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        for i in range(phnum):
            p_type, _fl, off, va, _pa, fsz, msz, _al = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
            if p_type != 1 or not msz:
                continue
            lo, hi = va & ~0xFFF, (va + msz + 0xFFF) & ~0xFFF
            for page in range(lo, hi, 0x1000):
                if not any(r[0] <= page < r[1] + 1 for r in self.uc.mem_regions()):
                    self.uc.mem_map(page, 0x1000, UC_PROT_ALL)
            self.uc.mem_write(va, data[off:off + fsz] + bytes(msz - fsz))
        # a hook's call to the function it replaces (ORIG_<address>) goes on in the original
        self.uc.mem_map(ORIGINALS, ORIGINALS_SPAN, UC_PROT_ALL)

        def original(uc, address, size, _):
            uc.reg_write(UC_ARM64_REG_PC, address - ORIGINALS + MAIN_BASE)
        self.uc.hook_add(UC_HOOK_CODE, original, begin=ORIGINALS, end=ORIGINALS + ORIGINALS_SPAN - 1)

    # ---------------------------------------------------------------- memory --
    def alloc(self, data_or_size, align=16):
        self.heap_top = (self.heap_top + align - 1) & ~(align - 1)
        addr = self.heap_top
        data = bytes(data_or_size) if not isinstance(data_or_size, int) else bytes(data_or_size)
        self.uc.mem_write(addr, data)
        self.heap_top += max(len(data), 1)
        return addr

    def cstr(self, s):
        return self.alloc(s.encode() + b"\0")

    def read(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def read_u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def read_cstr(self, addr, limit=4096):
        b = self.read(addr, limit)
        return b[:b.index(b"\0")].decode("latin1")

    def reg(self, n):
        return self.uc.reg_read(UC_ARM64_REG_X0 + n)

    def set_reg(self, n, v):
        self.uc.reg_write(UC_ARM64_REG_X0 + n, v & 0xFFFFFFFFFFFFFFFF)

    def vreg(self, n):
        """V<n> as a 128-bit integer (a float result is its low 32 bits, a double its low 64)."""
        return self.uc.reg_read(UC_ARM64_REG_Q0 + n)

    def set_vreg(self, n, v):
        self.uc.reg_write(UC_ARM64_REG_Q0 + n, v & ((1 << 128) - 1))

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def host_function(self, name, handler):
        """A guest-callable function that runs handler(emu) in Python and returns its result in X0:
        for fake objects in tests (a vtable slot, a callback). Calls are recorded in calls by name."""
        idx = len(self.symbols) + 1 + len(self.host_funcs)
        if (idx + 1) * 16 >= self.n_stub_slots * 16 - 0x100:
            raise ValueError("too many host functions")
        self.host_funcs[idx] = (name, handler)
        return STUB_BASE + idx * 16

    def stub_function(self, addr, name, handler):
        """Replaces the original function at addr with handler(emu) (its result goes to X0): for
        code a test doesn't exercise but the code under test calls, like species data lookups
        that need the game's data loaded. Calls are recorded in calls by name. A function the
        patch module replaces (replacements, set by decomp/tools/difftest.py) is faked there too,
        since decompiled code calls the replacement directly."""
        target = self.host_function(name, handler)
        # ldr x16, #8; br x16; .quad target
        self.uc.mem_write(addr, struct.pack("<IIQ", 0x58000050, 0xD61F0200, target))
        if addr in self.replacements:
            self.redirect(self.replacements[addr], target)

    def redirect(self, addr, target):
        """Code that reaches addr continues at target instead, with the code there left alone
        (functions in the patch module can be shorter than a stub)."""
        def hook(uc, address, size, _):
            uc.reg_write(UC_ARM64_REG_PC, target)
        self.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

    # ----------------------------------------------------------------- calls --
    def _on_stub(self, uc, addr, size, _):
        if addr == RETURN_MAGIC:
            uc.emu_stop()
            return
        idx = (addr - STUB_BASE) // 16
        if idx in self.host_funcs:
            name, h = self.host_funcs[idx]
        else:
            name = self.symbols[idx]["name"] if idx < len(self.symbols) else "?"
            h = self.import_handlers.get(name)
        self.calls.append(name)
        if h is None:
            self.error = "unhandled import %s (called from 0x%x)" % (name, uc.reg_read(UC_ARM64_REG_LR) - 4)
            uc.emu_stop()
            return
        ret = h(self)
        if ret is not None:
            self.set_reg(0, ret)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def call(self, addr, *args, until=None, max_insns=50_000_000, x8=None, fargs=()):
        """Calls a guest function with integer/pointer args (X0..X7, then the stack); returns X0.
        fargs: floating-point args as raw bit patterns (V0..V7; a float's bits are its low 32).
        x8: where a function returning a large struct writes it. With until=addr, stops when
        that instruction is reached instead."""
        if x8 is not None:
            self.set_reg(8, x8)
        for i, v in enumerate(fargs):
            self.set_vreg(i, v)
        sp = STACK_BASE + STACK_SIZE - 0x100
        if len(args) > 8:                            # the rest go on the stack, 8 bytes each (AAPCS64)
            sp -= (8 * (len(args) - 8) + 15) & ~15
            for i, v in enumerate(args[8:]):
                self.uc.mem_write(sp + 8 * i, struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))
        for i, v in enumerate(args[:8]):
            self.set_reg(i, v)
        self.uc.reg_write(UC_ARM64_REG_SP, sp)
        self.uc.reg_write(UC_ARM64_REG_LR, RETURN_MAGIC)
        self.error = None
        try:
            self.uc.emu_start(addr, until if until is not None else RETURN_MAGIC + 4, count=max_insns)
        except UcError as e:
            raise RuntimeError("guest fault at 0x%x: %s" % (self.uc.reg_read(UC_ARM64_REG_PC), e))
        if self.error:
            raise RuntimeError(self.error)
        return self.reg(0)
