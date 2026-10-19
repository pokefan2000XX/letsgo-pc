"""decomp/src/pokemon/evolution.cpp and the Pokémon object's evolution and form methods against the
originals, with the evolution lists faked (pkm.fake_evolutions) and a fake party and checker."""
import struct

import pkm
import test_pokemon as tp

U8, U16, U32, BOOL = 0xFF, 0xFFFF, 0xFFFFFFFF, 0xFF

HAS_EVOLUTIONS, LEVEL_UP_EVOLUTION, CHECK_LEVEL_UP = 0x7100722A30, 0x7100722A80, 0x7100722B90
ITEM_EVOLUTION, CHECK_ITEM, TRADE_EVOLUTION, CHECK_TRADE = 0x7100723020, 0x7100723150, 0x7100723220, 0x7100723340
MEGA_FORM, CAN_MEGA_EVOLVE_TO, IS_MEGA_STONE = 0x7100723430, 0x7100723580, 0x71007236E0
# the Pokémon object
CAN_EVOLVE, GET_LEVEL_UP_EVOLUTION, GET_ITEM_EVOLUTION = 0x7100728140, 0x7100728170, 0x71007281E0
GET_TRADE_EVOLUTION, EVOLVE, CHANGE_SPECIES, CHANGE_FORM = 0x7100728250, 0x71007282C0, 0x71007283C0, 0x71007285D0
FIX_GENDER_FOR_FORM, MEGA_EVOLVE, CAN_MEGA_EVOLVE, MEGA_EVOLVE_TO = 0x71007286B0, 0x7100728750, 0x71007287F0, 0x7100728830
REVERT_MEGA, CLEAR_MEGA, TYPE1, TYPE2 = 0x71007288B0, 0x7100728DA0, 0x7100728B60, 0x7100728C00

ACC_SET_SPECIES, ACC_SET_FORM, ACC_SET_ABILITY, ACC_SET_HELD_ITEM = 0x71007317B0, 0x71007347A0, 0x7100731E70, 0x71007318F0
ACC_SET_GENDER, ACC_SET_UNK100, ACC_SET_UNK101 = 0x7100734660, 0x7100731570, 0x7100731690
SPECIES = [64, 493, 588, 616, 646, 670, 678, 773]
CHECKER = [0]


def setup(emu):
    tp.setup(emu)
    pkm.fake_evolutions(emu)
    blocks = emu.host_function("EvolutionChecker::BlocksEvolution", lambda e: (e.reg(1) & 0xFFFF) % 7 == 0)
    nothing = emu.host_function("EvolutionChecker virtual", lambda e: 0)
    vtable = emu.alloc(struct.pack("<6Q", nothing, nothing, nothing, nothing, nothing, blocks))
    CHECKER[0] = emu.alloc(struct.pack("<Q", vtable) + b"\0" * 0x58)
    emu.write(tp.SYSTEM[0] + 0x300, struct.pack("<Q", CHECKER[0]))


def mon(t):
    """A Pokémon (tp.Mon), often one of the species the code checks for, with a small held item,
    an ability that matters and a Mega Evolution state."""
    m = tp.Mon(t, *t.rng.choice(pkm.STATES))
    r = m.rec.obj
    if t.rng.random() < 0.7:
        t.call(ACC_SET_SPECIES, r, t.rng.choice(SPECIES + [t.rng.randrange(1, 900)]))
    if t.rng.random() < 0.5:
        t.call(ACC_SET_FORM, r, t.rng.randrange(6))
    if t.rng.random() < 0.6:
        t.call(ACC_SET_HELD_ITEM, r, t.rng.randrange(12))
    if t.rng.random() < 0.5:
        t.call(ACC_SET_ABILITY, r, t.rng.choice([121, 225, t.rng.randrange(300)]))
    if t.rng.random() < 0.5:
        t.call(ACC_SET_GENDER, r, t.rng.randrange(3))
    if t.rng.random() < 0.4:
        t.call(ACC_SET_UNK100, r, t.rng.randrange(2))
        t.call(ACC_SET_UNK101, r, t.rng.randrange(4))
    return m


def party(t):
    """A party object: GetMember (vtable slot 11) returns these members, then null."""
    members = [mon(t).obj for _ in range(t.rng.randrange(7))]
    get = t.emu.host_function("Party::GetMember", lambda e: members[e.reg(1) & 0xFFFFFFFF]
                              if (e.reg(1) & 0xFFFFFFFF) < len(members) else 0)
    vtable = t.emu.alloc(struct.pack("<12Q", *([0] * 11 + [get])))
    obj = t.emu.alloc(0x90)
    t.emu.write(obj, struct.pack("<Q", vtable))
    t.emu.write(obj + 0x88, bytes([len(members)]))
    return obj


def context(t, m):
    """An EvolutionContext: random flags, and the Pokémon's original trainer or another."""
    flags = bytes(t.rng.choice([0, 0, 1, 2]) for _ in range(12))
    info = t.emu.read(tp.trainer(t, m), 0x20)
    return t.emu.alloc(flags + info)


def entry(t):
    return t.emu.alloc(struct.pack("<IHHIB3x", t.rng.randrange(-1, 46) & U32, t.rng.randrange(12), 0,
                                   t.rng.randrange(900), t.rng.randrange(60)))


def test_evolution_checks(t):
    for _ in range(40):
        m, other = mon(t), mon(t)
        p, ctx = party(t), context(t, m)
        t.emu.write(tp.SYSTEM[0] + 0x30C, bytes([t.rng.randrange(4)]))
        checker = CHECKER[0]
        for _ in range(4):
            e = entry(t)
            t.check(t.call(CHECK_LEVEL_UP, checker, m.obj, p, ctx, e) & BOOL)
            t.check(t.call(CHECK_ITEM, checker, m.obj, ctx, t.rng.randrange(12), e) & BOOL)
            t.check(t.call(CHECK_TRADE, checker, m.obj, t.rng.choice([0, other.obj]), e) & BOOL)
        t.check(t.call(HAS_EVOLUTIONS, checker, m.obj) & BOOL)
        index = t.emu.alloc(struct.pack("<I", 0xEEEE))
        t.check((t.call(LEVEL_UP_EVOLUTION, checker, m.obj, p, ctx, t.rng.choice([0, index])) & U16,
                 t.emu.read(index, 4)))
        t.check((t.call(ITEM_EVOLUTION, checker, m.obj, ctx, t.rng.randrange(12), t.rng.choice([0, index])) & U16,
                 t.emu.read(index, 4)))
        t.check((t.call(TRADE_EVOLUTION, checker, m.obj, t.rng.choice([0, other.obj]), index) & U16,
                 t.emu.read(index, 4)))
        t.check(t.call(MEGA_FORM, checker, m.obj) & U8)
        for form in range(4):
            t.check(t.call(CAN_MEGA_EVOLVE_TO, checker, m.obj, form) & BOOL)
        for item in range(11):
            t.check(t.call(IS_MEGA_STONE, checker, t.rng.choice(SPECIES + [t.rng.randrange(900)]), item) & BOOL)
        t.check(m.snapshot())


def test_pokemon_evolution(t):
    for _ in range(40):
        m, other = mon(t), mon(t)
        p, ctx = party(t), context(t, m)
        species, index = t.emu.alloc(4), t.emu.alloc(struct.pack("<I", 0xEEEE))
        t.check(t.call(CAN_EVOLVE, m.obj) & BOOL)
        t.check((t.call(GET_LEVEL_UP_EVOLUTION, m.obj, ctx, p, species, index) & BOOL,
                 t.emu.read(species, 4), t.emu.read(index, 4)))
        t.check((t.call(GET_ITEM_EVOLUTION, m.obj, ctx, t.rng.choice([3, 0x10005]), species, index) & BOOL,
                 t.emu.read(species, 4), t.emu.read(index, 4)))
        t.check((t.call(GET_TRADE_EVOLUTION, m.obj, other.obj, species, index) & BOOL,
                 t.emu.read(species, 4), t.emu.read(index, 4)))
        t.check((t.call(TYPE1, m.obj) & U8, t.call(TYPE2, m.obj) & U8))
        t.call(EVOLVE, m.obj, t.rng.choice(SPECIES + [t.rng.randrange(900)]), t.rng.randrange(4))
        t.check(m.snapshot())


def test_species_and_forms(t):
    for _ in range(40):
        m = mon(t)
        t.call(CHANGE_SPECIES, m.obj, t.rng.choice(SPECIES + [t.rng.randrange(900)]), t.rng.randrange(4))
        t.check(m.snapshot())
        t.call(CHANGE_FORM, m.obj, t.rng.choice([0, 1, 2, 3, 0x102]))
        t.check(m.snapshot())
        t.check((t.call(FIX_GENDER_FOR_FORM, m.obj) & BOOL, m.snapshot()))
        t.check(t.call(CAN_MEGA_EVOLVE, m.obj) & BOOL)
        t.call(MEGA_EVOLVE, m.obj)
        t.check(m.snapshot())
        t.call(REVERT_MEGA, m.obj)
        t.check(m.snapshot())
        t.call(MEGA_EVOLVE_TO, m.obj, t.rng.randrange(4))
        t.check(m.snapshot())
        t.call(t.rng.choice([REVERT_MEGA, CLEAR_MEGA]), m.obj)
        t.check(m.snapshot())


def test_trade_pairs(t):
    for a, b in [(616, 588), (588, 616), (616, 616), (588, 588), (64, 588), (616, 64)]:
        for _ in range(4):
            m, other = mon(t), mon(t)
            t.call(ACC_SET_SPECIES, m.rec.obj, a)
            t.call(ACC_SET_SPECIES, other.rec.obj, b)
            e = t.emu.alloc(struct.pack("<IHHIB3x", 7, 0, 0, 1, 0))
            t.check((a, b, t.call(CHECK_TRADE, CHECKER[0], m.obj, other.obj, e) & BOOL))
