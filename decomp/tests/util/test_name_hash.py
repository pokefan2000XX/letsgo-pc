"""decomp/src/util/name_hash.cpp against the originals (decomp/tools/difftest.py)."""
import struct

NAME_EQUALS = 0x7100101B30
GET_NAME = 0x7100101BA0

FIXED = [("bin/archive/field/area01.gfpak", "bin/archive/field/area01.gfpak"), ("pm0025_00", "pm0133_00"),
         ("", ""), ("", "x"), ("x", ""), ("\xff\x80", "\xff\x80"), ("a" * 300, "a" * 299 + "b")]


def named_object(t, name):
    obj = t.emu.alloc(0x100)
    t.emu.uc.mem_write(obj + 0x88, struct.pack("<Q", t.emu.alloc(name.encode("latin1") + b"\0")))
    return obj


def random_name(t):
    return "".join(chr(t.rng.randrange(1, 256)) for _ in range(t.rng.randrange(0, 40)))


def test_name_equals(t):
    pairs = list(FIXED)
    for _ in range(200):
        a = random_name(t)
        pairs.append((a, a if t.rng.random() < 0.3 else random_name(t)))
    for a, b in pairs:
        obj = named_object(t, a)
        t.check(t.call(NAME_EQUALS, obj, t.emu.alloc(b.encode("latin1") + b"\0")) & 1)


def test_get_name(t):
    for a, _ in FIXED:
        obj = named_object(t, a)
        t.check(t.emu.read_cstr(t.call(GET_NAME, obj)))
