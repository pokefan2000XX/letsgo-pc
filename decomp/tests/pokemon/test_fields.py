"""decomp/src/pokemon/accessor_fields.cpp against the originals: every field accessor in every
record state (encrypted, fast mode, decrypted; with and without party stats; bad eggs)."""
import struct

import pkm

U8, U16, U32, BOOL = 0xFF, 0xFFFF, 0xFFFFFFFF, 0xFF

# getters without arguments: (address, return mask)
CALC_GETTERS = [(0x7100729BF0, U32), (0x710072A4F0, U8), (0x7100729D10, U16), (0x7100729E30, U16),
                (0x7100729F50, U16), (0x710072A070, U16), (0x710072A190, U16), (0x710072A2B0, U16),
                (0x710072A3D0, U16), (0x710072A610, U16), (0x710072A870, BOOL), (0x710072A9A0, U8),
                (0x710072A730, U8)]
BLOCK_GETTERS = [
    (0x710072AAC0, U16), (0x710072ABF0, U16), (0x710072AD20, U32), (0x710072AE50, U32), (0x710072AF80, U8),
    (0x710072B130, U8), (0x710072D730, BOOL), (0x710072D860, BOOL), (0x710072D990, BOOL), (0x710072B260, U16),
    (0x710072DAC0, U32), (0x710072D600, U8), (0x710072D290, BOOL), (0x710072D3C0, U8), (0x710072D4E0, U8),
    (0x710072B4C0, U8), (0x710072B5F0, U8), (0x710072B720, U8), (0x710072B850, U8), (0x710072B980, U8),
    (0x710072BAB0, U8), (0x710072BBE0, U8), (0x710072BD10, U8), (0x710072BE40, U8), (0x710072BF70, U8),
    (0x710072C0A0, U8), (0x710072C1D0, U8), (0x710072EBF0, U8), (0x710072FB50, U8), (0x710072FC80, U8),
    (0x7100738190, U32), (0x710072C970, U8), (0x710072CA90, U8), (0x710072CBB0, U8), (0x710072CCD0, U8),
    (0x710072CDF0, U8), (0x710072CF10, U8), (0x710072D030, BOOL), (0x710072D160, BOOL), (0x710072F090, U32),
    (0x710072F1C0, U8), (0x710072F2F0, U8), (0x710072F420, U8), (0x710072F680, U8), (0x710072F550, U16),
    (0x710072FDB0, U8), (0x710072FEE0, U8), (0x7100730010, U8), (0x7100730140, U8), (0x7100730270, U8),
    (0x71007303A0, U8), (0x71007304D0, U8), (0x7100730600, U8), (0x710072E270, U8), (0x710072E3A0, U8),
    (0x710072E4D0, U8), (0x710072E600, U8), (0x710072E730, U8), (0x710072E860, U8), (0x710072C300, U8),
    (0x710072E990, U16), (0x710072EAC0, U16), (0x710072ED20, U8), (0x710072EE50, U8), (0x710072EF70, U8),
    (0x7100738400, U8), (0x710072DED0, U8), (0x710072B390, U8)]
FLOAT_GETTERS = [0x710072F8F0, 0x710072FA20]
# setters with one argument: (address, value bits)
CALC_SETTERS = [(0x71007308F0, 32), (0x7100730A10, 8), (0x7100730C50, 16), (0x7100730B30, 16), (0x7100730D70, 16),
                (0x7100730E90, 16), (0x7100730FB0, 16), (0x71007310D0, 16), (0x71007311F0, 16), (0x7100731310, 16),
                (0x7100731570, 8), (0x7100731690, 8), (0x7100731430, 8), (0x7100738530, 8), (0x7100738650, 8)]
BLOCK_SETTERS = [
    (0x71007317B0, 16), (0x71007318F0, 16), (0x7100731A30, 32), (0x7100731B70, 32), (0x7100731CB0, 8),
    (0x7100731E70, 8), (0x7100734A10, 8), (0x7100734B50, 8), (0x7100734C90, 8), (0x7100734DD0, 8),
    (0x7100731FB0, 16), (0x7100734F10, 32), (0x71007348D0, 8), (0x7100734520, 8), (0x7100734660, 8),
    (0x71007347A0, 8), (0x71007325F0, 8), (0x7100732730, 8), (0x7100732870, 8), (0x71007329B0, 8),
    (0x7100732AF0, 8), (0x7100732C30, 8), (0x7100732D70, 8), (0x7100732EB0, 8), (0x7100732FF0, 8),
    (0x7100733130, 8), (0x7100733270, 8), (0x71007333B0, 8), (0x7100737080, 8), (0x71007371C0, 8),
    (0x7100738050, 32), (0x7100733B30, 8), (0x7100733C60, 8), (0x7100733DA0, 8), (0x7100733EE0, 8),
    (0x7100734020, 8), (0x7100734160, 8), (0x71007342A0, 8), (0x71007343E0, 8), (0x7100736CC0, 8),
    (0x7100736190, 8), (0x71007362D0, 32), (0x7100736550, 8), (0x7100736690, 8), (0x71007367D0, 8),
    (0x7100736A50, 8), (0x7100736910, 16), (0x7100737300, 8), (0x7100737440, 8), (0x7100736410, 8),
    (0x7100737580, 8), (0x71007376C0, 8), (0x7100737800, 8), (0x7100737940, 8), (0x7100737A80, 8),
    (0x7100737BC0, 8), (0x71007353F0, 8), (0x7100735530, 8), (0x7100735670, 8), (0x71007357B0, 8),
    (0x71007358F0, 8), (0x7100735A30, 8), (0x71007334F0, 8), (0x7100735B70, 16), (0x7100735CB0, 16),
    (0x7100735DF0, 8), (0x7100735F30, 8), (0x7100736060, 8), (0x71007382C0, 8), (0x7100735180, 8),
    (0x71007320F0, 8), (0x7100732230, 8), (0x7100732370, 8), (0x71007324B0, 8)]
FLOAT_SETTERS = [0x7100736E00, 0x7100736F40]
# slot accessors: (getter, return mask, setter, value bits)
SLOTS = [(0x710072C430, U16, 0x7100733630, 16), (0x710072C580, U8, 0x7100733770, 8),
         (0x710072C6D0, U8, 0x71007338B0, 8), (0x710072C820, U16, 0x71007339F0, 16)]
HAS_RIBBON, SET_RIBBON = 0x7100737D00, 0x7100737EB0
SET_KEY, SET_BAD_EGG = 0x7100730730, 0x71007308D0
GET_NICKNAME_BUF, GET_NICKNAME, SET_NICKNAME = 0x710072DBF0, 0x710072DD50, 0x7100735050
GET_OT_NAME_BUF, GET_OT_NAME, IS_OT_NAME, SET_OT_NAME = 0x710072E000, 0x710072E130, 0x710072F7B0, 0x71007352C0
SET_HANDLER_NAME = 0x7100736B90
SPECIES_NAME_BUF, SPECIES_NAME = 0x710071FA10, 0x710071F980


def setup(emu):
    emu.call(pkm.INIT_DEFAULT_RECORDS)

    def name_of(species, language):
        return [0x42 + species % 7, 0x61 + language % 5, 0x64, 0x45, 0x67, 0x67, 0]

    def species_name_buf(e):        # writes into a StrBuf's buffer (see str_buf)
        out, species, language = e.reg(0), e.reg(1) & 0xFFFFFFFF, e.reg(2) & 0xFFFFFFFF
        buf = e.read_u64(out + 0x58)
        e.write(buf, struct.pack("<7H", *name_of(species, language)))
        e.write(out + 0x7A, struct.pack("<H", 6))
        return 1

    def species_name(e):
        out, species, language = e.reg(0), e.reg(1) & 0xFFFFFFFF, e.reg(2) & 0xFFFFFFFF
        e.write(out, struct.pack("<7H", *name_of(species, language)))
        return 1

    emu.stub_function(SPECIES_NAME_BUF, "GetSpeciesName(StrBuf*)", species_name_buf)
    emu.stub_function(SPECIES_NAME, "GetSpeciesName(u16*)", species_name)


def records(t, rounds):
    for _ in range(rounds):
        for state, calc, bad in pkm.STATES:
            yield pkm.Record(t, state, calc, bad)


def test_calc_fields(t):
    for r in records(t, 3):
        for addr, mask in CALC_GETTERS:
            t.check((hex(addr), t.call(addr, r.obj) & mask, r.snapshot()))
        for addr, bits in CALC_SETTERS:
            t.call(addr, r.obj, t.rng.getrandbits(bits))
            t.check((hex(addr), r.snapshot()))


def test_block_getters(t):
    for r in records(t, 3):
        for addr, mask in BLOCK_GETTERS:
            t.check((hex(addr), t.call(addr, r.obj) & mask, r.snapshot()))
        for addr in FLOAT_GETTERS:
            t.call(addr, r.obj)
            t.check((hex(addr), t.emu.vreg(0) & 0xFFFFFFFF, r.snapshot()))


def test_block_setters(t):
    for r in records(t, 3):
        for addr, bits in BLOCK_SETTERS:
            t.call(addr, r.obj, t.rng.getrandbits(bits))
            t.check((hex(addr), r.snapshot()))
        for addr in FLOAT_SETTERS:
            t.call(addr, r.obj, fargs=[t.rng.getrandbits(32)])
            t.check((hex(addr), r.snapshot()))
        # read back after the writes
        for addr, mask in BLOCK_GETTERS[::5]:
            t.check((hex(addr), t.call(addr, r.obj) & mask))


def test_slots(t):
    for r in records(t, 2):
        for getter, mask, setter, bits in SLOTS:
            for slot in (0, 1, 2, 3, 4, 7, 0x103):
                t.check((hex(getter), slot, t.call(getter, r.obj, slot) & mask, r.snapshot()))
                t.call(setter, r.obj, slot, t.rng.getrandbits(bits))
                t.check((hex(setter), slot, r.snapshot()))


def test_ribbons(t):
    for r in records(t, 2):
        for ribbon in list(range(0, 70, 3)) + [31, 32, 63, 64, 0xFFFFFFFF]:
            t.check((ribbon, t.call(HAS_RIBBON, r.obj, ribbon) & BOOL, r.snapshot()))
            if t.rng.random() < 0.5:
                t.call(SET_RIBBON, r.obj, ribbon)
                t.check((ribbon, r.snapshot()))


def str_buf(t, capacity):
    buf = t.emu.alloc(b"\xcc" * 0x40)
    obj = t.emu.alloc(0x80)
    t.emu.write(obj + 0x58, struct.pack("<Q", buf))
    t.emu.write(obj + 0x78, struct.pack("<HHB", capacity, 0, 1))
    return obj, buf


def random_name(t):
    n = t.rng.randrange(0, 14)
    return struct.pack("<%dH" % n, *[t.rng.randrange(0x20, 0x3000) for _ in range(n)]) + b"\0\0"


def test_names(t):
    for r in records(t, 3):
        for getter in (GET_NICKNAME_BUF, GET_OT_NAME_BUF):
            obj, buf = str_buf(t, t.rng.choice([13, 6]))
            t.call(getter, r.obj, obj)
            t.check((hex(getter), t.emu.read(obj + 0x78, 6), t.emu.read(buf, 0x40), r.snapshot()))
        for getter in (GET_NICKNAME, GET_OT_NAME):
            for size in (13, 5, 0, 20):
                out = t.emu.alloc(b"\xcc" * 0x40)
                t.call(getter, r.obj, out, size)
                t.check((hex(getter), size, t.emu.read(out, 0x40), r.snapshot()))
        out = t.emu.alloc(0x40)
        t.call(GET_OT_NAME, r.obj, out, 13)
        t.check(t.call(IS_OT_NAME, r.obj, out) & BOOL)
        t.check(t.call(IS_OT_NAME, r.obj, t.emu.alloc(random_name(t))) & BOOL)
        for setter in (SET_NICKNAME, SET_OT_NAME, SET_HANDLER_NAME):
            t.call(setter, r.obj, t.emu.alloc(random_name(t)))
            t.check((hex(setter), r.snapshot()))


def test_key_and_bad_egg(t):
    for r in records(t, 3):
        t.call(SET_KEY, r.obj, t.rng.getrandbits(32))
        t.check(r.snapshot())
        t.call(SET_BAD_EGG, r.obj, t.rng.randrange(4))
        t.check(r.snapshot())
        t.check(t.call(0x710072AAC0, r.obj) & U16)
