"""decomp/src/pokemon/record.cpp against the originals."""
import struct

import pkm

ENCODE, DECODE, CHECKSUM = 0x7100739420, 0x71007394B0, 0x71007393B0
CREATE_CORE, CREATE_CALC = 0x7100739540, 0x7100739FC0
CALC_HP, CALC_STATS = 0x7100738870, [0x71007388B0, 0x7100738970, 0x7100738A30, 0x7100738AF0, 0x7100738BB0]
NATURE_MODIFIER, LEVEL_FROM_EXP = 0x7100738C70, 0x7100738800
CALC_CP, CALC_BASE_CP = 0x7100738CA0, 0x7100738DA0
IS_SHINY, MAKE_NON_SHINY, MAKE_SHINY = 0x7100739050, 0x7100739070, 0x71007390A0
VALID_GENDER, ABILITY_ID, HIDDEN_POWER, NATURE_FLAVOR = 0x71007390E0, 0x7100739140, 0x7100739190, 0x71007391E0
PLATE_TYPE, MEMORY_TYPE = 0x7100739200, 0x7100739370
GET_PAIR, SET_PAIR, SET_HIGH, SET_LOW = 0x710073A0C0, 0x710073A0E0, 0x710073A110, 0x710073A120


def setup(emu):
    pkm.fake_personal(emu)


def test_crypt_and_checksum(t):
    for size in (0xE0, 0x1C, 0x104, 0, 1, 2, 3, 0x37):
        for _ in range(6):
            data = bytes(t.rng.randrange(256) for _ in range(size)) + b"\xa5\x5a"
            buf = t.emu.alloc(data)
            seed = t.rng.getrandbits(32)
            t.call(ENCODE, buf, size, seed)
            t.check(t.emu.read(buf, size + 2))
            t.check(t.call(CHECKSUM, buf, size) & 0xFFFF)
            t.call(DECODE, buf, size, seed)
            t.check(t.emu.read(buf, size + 2))


def test_default_records(t):
    t.emu.write(pkm.DEFAULTS, b"\x77" * pkm.DEFAULTS_SIZE)
    t.call(pkm.INIT_DEFAULT_RECORDS)
    t.check(t.emu.read(pkm.DEFAULTS, pkm.DEFAULTS_SIZE))


def test_create_records(t):
    res = pkm.FakeResource(t.emu)
    for depth in (0, 3, 15):
        t.emu.write(res.stack, b"\x11" * 0x80 + struct.pack("<Q", depth))
        out = t.emu.alloc(0x20)
        t.call(CREATE_CORE, res.addr, x8=out)
        ptr, owner, size, align = struct.unpack("<4Q", t.emu.read(out, 0x20))
        t.check((owner == res.addr, size, align, t.emu.read(ptr, pkm.CORE_SIZE), t.emu.read(res.stack, 0x88)))
        key = t.rng.getrandbits(32)
        t.call(CREATE_CALC, res.addr, key, x8=out)
        ptr, owner, size, align = struct.unpack("<4Q", t.emu.read(out, 0x20))
        t.check((owner == res.addr, size, align, t.emu.read(ptr, pkm.CALC_SIZE), t.emu.read(res.stack, 0x88)))
    t.check(res.log)


def test_stats(t):
    cases = [(1, 1, 0, 0, 0, 0), (100, 255, 31, 0, 200, 255), (100, 255, 31, 0, 200, 0)]
    for _ in range(300):
        cases.append((t.rng.randrange(256), t.rng.randrange(0x10000) if t.rng.random() < 0.1 else t.rng.randrange(256),
                      t.rng.randrange(32), t.rng.randrange(25), t.rng.randrange(256), t.rng.randrange(256)))
    for level, base, iv, nature, av, friendship in cases:
        for species in (292, 25, 0x10124):
            t.check(t.call(CALC_HP, species, level, base, iv, av) & 0xFFFF)
        for fn in CALC_STATS:
            t.check(t.call(fn, level, base, iv, nature, av, t.rng.randrange(256), friendship) & 0xFFFF)
    for nature in range(25):
        for stat in range(-1, 8):
            t.check(t.call(NATURE_MODIFIER, nature, stat & 0xFFFFFFFF) & 0xFF)


def test_combat_power(t):
    for _ in range(200):
        level = t.rng.randrange(1, 101) if t.rng.random() < 0.9 else t.rng.randrange(256)
        nature, friendship = t.rng.randrange(25), t.rng.randrange(256)
        bases = [t.rng.randrange(1, 256) for _ in range(6)]
        ivs = [t.rng.randrange(32) for _ in range(6)]
        avs = [t.rng.randrange(201) if t.rng.random() < 0.9 else t.rng.randrange(0x10000) for _ in range(6)]
        t.check(t.call(CALC_BASE_CP, level, nature, *bases, *ivs, friendship) & 0xFFFFFFFF)
        t.check(t.call(CALC_CP, level, nature, t.rng.getrandbits(32), friendship, *bases, *ivs, *avs) & 0xFFFF)


def test_level_from_exp(t):
    for _ in range(150):
        species, form = t.rng.randrange(900), t.rng.randrange(4)
        exp = t.rng.choice([0, 1, 7, 8, 1000000, 1250000, 0xFFFFFFFF, t.rng.randrange(1300000)])
        t.check(t.call(LEVEL_FROM_EXP, species, form, exp) & 0xFF)


def test_shiny(t):
    for _ in range(400):
        tid = t.rng.getrandbits(32)
        pid = t.rng.getrandbits(32)
        if t.rng.random() < 0.3:        # shiny: make the halves XOR below 16
            pid = (pid & 0xFFFF) | ((((tid >> 16) ^ tid ^ pid) & 0xFFF0 | t.rng.randrange(16)) << 16)
        t.check(t.call(IS_SHINY, tid, pid) & 1)
        t.check(t.call(MAKE_NON_SHINY, tid, pid) & 0xFFFFFFFF)
        t.check(t.call(MAKE_SHINY, tid, pid) & 0xFFFFFFFF)


def test_species_lookups(t):
    for species in range(40):
        for form in range(2):
            for gender in range(4):
                t.check(t.call(VALID_GENDER, species, form, gender) & 0xFFFFFFFF)
            for slot in range(4):
                t.check(t.call(ABILITY_ID, species, form, slot) & 0xFFFF)


def test_tables(t):
    for _ in range(100):
        ivs = [t.rng.randrange(256) for _ in range(6)]
        t.check(t.call(HIDDEN_POWER, *ivs) & 0xFF)
    for nature in range(25):
        for flavor in range(5):
            t.check(t.call(NATURE_FLAVOR, nature, flavor) & 0xFFFFFFFF)
    for item in list(range(0x400)) + [0x1012A, 0x10388, 0xFFFFFFFF]:
        t.check((t.call(PLATE_TYPE, item) & 0xFF, t.call(MEMORY_TYPE, item) & 0xFF))


def test_bit_helpers(t):
    for _ in range(200):
        bits, value, index = t.rng.randrange(0x10000), t.rng.randrange(0x10000), t.rng.randrange(8)
        word = t.rng.getrandbits(32)
        t.check(t.call(GET_PAIR, bits, index) & 0xFFFF)
        t.check(t.call(SET_PAIR, bits, index, value & 3) & 0xFFFF)
        t.check(t.call(SET_HIGH, word, value) & 0xFFFFFFFF)     # callers keep all 32 bits
        t.check(t.call(SET_LOW, word, value) & 0xFFFFFFFF)
