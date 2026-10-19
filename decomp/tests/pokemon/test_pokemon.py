"""decomp/src/pokemon/pokemon.cpp against the originals: the Pokémon object's methods over records in
every state, with species data faked (pkm.fake_personal)."""
import struct

import pkm

U8, U16, U32, BOOL = 0xFF, 0xFFFF, 0xFFFFFFFF, 0xFF

# one-line wrappers over the accessor
GETTERS = [(0x71007247C0, U16), (0x7100724B60, U8), (0x7100724B70, U8), (0x7100724B90, U8), (0x7100724BB0, U8),
           (0x7100724E40, U16), (0x7100726780, U32), (0x71007268C0, U8), (0x71007269A0, U32), (0x7100727AB0, U8),
           (0x7100727AC0, U8), (0x7100727B00, U8), (0x7100727B20, BOOL), (0x7100727CA0, BOOL), (0x7100727E20, U8),
           (0x7100727E40, U8), (0x7100727F90, U32), (0x7100728130, U32), (0x7100728900, BOOL), (0x7100728B50, U32),
           (0x7100728CE0, BOOL), (0x7100728E00, U8), (0x7100728E10, U8), (0x7100728FB0, U8), (0x7100728FC0, U8),
           (0x7100728FD0, U8), (0x7100728FF0, U8), (0x7100729010, U8), (0x7100729030, U8), (0x7100729050, U8),
           (0x7100729070, U8), (0x71007247D0, U16), (0x71007247A0, BOOL)]
FLOAT_GETTERS = [0x7100728DE0, 0x7100728DF0]
SETTERS = [(0x7100724040, 32), (0x71007242A0, 32), (0x71007242B0, 32), (0x71007247F0, 32), (0x7100724B80, 32),
           (0x7100724BA0, 32), (0x71007269B0, 32), (0x7100727B10, 32), (0x7100727E30, 32), (0x7100727FA0, 32),
           (0x7100728B40, 32), (0x7100728CF0, 32), (0x7100728FE0, 32), (0x7100729000, 32), (0x7100729020, 32),
           (0x7100729040, 32), (0x7100729060, 32), (0x7100729080, 32)]
NO_ARGS = [0x71007269C0, 0x7100724790, 0x7100728120]       # ClearStatus, Clear, MarkBadEgg
SLOT_GETTERS = [(0x7100726CC0, U16), (0x7100726DD0, U16), (0x71007269D0, U8)]
HAS_RIBBON, SET_RIBBON = 0x7100728CA0, 0x7100728CB0
NICKNAME_BUF, NICKNAME, OT_NAME_BUF, OT_NAME = 0x7100727CB0, 0x7100727CC0, 0x7100727DD0, 0x7100727DE0
SERIALIZE_CORE, DESERIALIZE_CORE, SERIALIZE, DESERIALIZE = 0x7100724770, 0x7100724780, 0x71007294C0, 0x71007294D0
SPECIES_NAME_BUF, SPECIES_NAME = 0x710071FA10, 0x710071F980

# markings and other small fields
HAS_MARKING, GET_MARKING, SET_MARKING, CLEAR_MARKING = 0x7100724800, 0x7100724850, 0x7100724880, 0x71007248F0
SET_ALL_MARKINGS, GET_ALL_MARKINGS = 0x7100724940, 0x7100724AC0
ADD_UNK_AE, SET_UNK3C_LOW, SET_UNK3C_HIGH = 0x7100724BC0, 0x7100724C00, 0x7100724C40
GET_MET_INFO, SET_MET_INFO = 0x7100724C80, 0x7100724D70
# IVs, EVs, AVs
GET_IV, GET_EFFECTIVE_IV, IS_HYPER_TRAINED, SET_IV = 0x7100724E50, 0x7100724EB0, 0x7100724F10, 0x7100724F70
HIDDEN_POWER = 0x71007254C0
GET_EV, GET_EV_TOTAL, ADD_EV, SET_EV = 0x7100725550, 0x71007255B0, 0x7100725620, 0x7100725670
SUBTRACT_EV, CLAMP_EV = 0x71007257A0, 0x71007257E0
GET_AV, ADD_AV, SET_AV, SET_UNK_D7 = 0x71007258A0, 0x7100725900, 0x7100725940, 0x7100725A60
# stats
GET_STAT, GET_MAX_HP, GET_HP = 0x7100725BD0, 0x7100725D10, 0x71007263F0
CALCS = [0x7100725D60, 0x7100725E50, 0x7100725F70, 0x7100726090, 0x71007261B0, 0x71007262D0]
UPDATE_MAX_HP, UPDATE_CP, RECALC_STATS = 0x7100725080, 0x7100725170, 0x7100725AA0
SET_HP, RECOVER_HP, IS_FULL_HP, IS_FAINTED, HAS_STATUS = 0x7100726440, 0x7100726480, 0x71007264D0, 0x7100726530, 0x7100726980
# level and EXP
GET_LEVEL, ADD_LEVELS, SET_EXP, ADD_EXP = 0x7100726580, 0x7100726600, 0x7100726710, 0x7100726790
GET_LEVEL_EXP, GET_NEXT_LEVEL_EXP = 0x7100726810, 0x71007268D0

# moves
SET_MOVE, CLEAR_MOVE, HAS_MOVE, MOVE_COUNT = 0x71007270F0, 0x7100726E00, 0x7100726D50, 0x7100726CD0
SWAP_MOVES, COMPACT_MOVES, LEARN_MOVE, PUSH_MOVE = 0x7100727180, 0x71007272B0, 0x7100727850, 0x7100726E70
RESET_MOVES, LEARN_LEVEL_UP_MOVE, LEARN_EVOLUTION_MOVE = 0x7100724050, 0x7100727670, 0x7100727980
UPDATE_KYUREM_MOVES, CAN_LEARN_MACHINE, SET_RELEARN_MOVE = 0x7100728910, 0x7100727370, 0x7100726DE0
SET_MOVE_PP, GET_MAX_PP, RECOVER_PP, RECOVER_ALL_PP = 0x71007269F0, 0x7100726A60, 0x7100726BB0, 0x7100726AB0
GET_PP_UPS, SET_PP_UPS = 0x7100726C80, 0x7100726CA0
MOVE_LIST_CLEAR, MOVE_LIST_CONTAINS = 0x71007273C0, 0x71007273D0
# names, trainers, friendship
SET_NICKNAME, CLEAR_NICKNAME, IS_NICKNAME_SPECIES_NAME = 0x7100727CD0, 0x71007241E0, 0x7100727D60
SET_OT_NAME, IS_ORIGINAL_TRAINER, SET_CURRENT_TRAINER = 0x7100727DF0, 0x7100727B30, 0x7100727BA0
SET_FRIENDSHIP, ADD_FRIENDSHIP, SUBTRACT_FRIENDSHIP = 0x7100727E50, 0x7100727E90, 0x7100727EE0
SET_OT_FRIENDSHIP, GET_HANDLER_FRIENDSHIP, SET_HANDLER_FRIENDSHIP = 0x7100727F30, 0x7100727F50, 0x7100727F70
GET_OT_GENDER, GET_ID = 0x7100727E20, 0x7100728130
# other state
SET_HYPER_TRAINED, NATURE_MODIFIER, NATURE_FLAVOR, HEAL = 0x7100725470, 0x7100729090, 0x7100727AD0, 0x7100724240
IS_EGG, IS_SHINY, HAS_POKERUS = 0x7100727FB0, 0x7100728B00, 0x7100728CC0
START_FAST_MODE, END_FAST_MODE = 0x7100728D00, 0x7100728D50
HEIGHT_CLASS, UPDATE_ABSOLUTE_SIZE, SET_ABSOLUTE_SIZE = 0x7100728E20, 0x71007242C0, 0x7100728E90
GET_SYSTEM = 0x710073B320
KYUREM_MOVES = [184, 558, 559, 549, 554, 553]

# accessor setters used to set records up
ACC_SET_HP, ACC_SET_EXP, ACC_SET_HYPER = 0x7100730C50, 0x7100731B70, 0x71007382C0
ACC_SET_LEVEL, ACC_SET_MOVE, ACC_SET_PP, ACC_SET_PP_UPS = 0x7100730A10, 0x7100733630, 0x7100733770, 0x71007338B0
ACC_SET_EVS = [0x71007325F0, 0x7100732730, 0x7100732870, 0x71007329B0, 0x7100732AF0, 0x7100732C30]


def setup(emu):
    pkm.fake_personal(emu)
    pkm.fake_moves(emu)
    emu.call(pkm.INIT_DEFAULT_RECORDS)

    def species_name_buf(e):
        out, species, language = e.reg(0), e.reg(1) & 0xFFFFFFFF, e.reg(2) & 0xFFFFFFFF
        e.write(e.read_u64(out + 0x58), struct.pack("<7H", *name_of(species, language)))
        e.write(out + 0x7A, struct.pack("<H", 6))
        return 1

    def species_name(e):
        e.write(e.reg(0), struct.pack("<7H", *name_of(e.reg(1) & 0xFFFFFFFF, e.reg(2) & 0xFFFFFFFF)))
        return 1

    emu.stub_function(SPECIES_NAME_BUF, "GetSpeciesName(StrBuf*)", species_name_buf)
    emu.stub_function(SPECIES_NAME, "GetSpeciesName(u16*)", species_name)
    system = emu.alloc(0x400)
    emu.stub_function(GET_SYSTEM, "GetSystem", lambda e: system)
    SYSTEM[0] = system


SYSTEM = [0]


def name_of(species, language):
    return [0x42 + species % 7, 0x61 + language % 5, 0x64, 0x45, 0x67, 0x67, 0]


class Mon:
    """A Pokémon object (0xB8 bytes) whose accessor is a pkm.Record. Most records are tuned to be
    plausible: a level below 100, EVs that fit the totals, sometimes fainted or hyper trained."""

    def __init__(self, t, state, calc, bad_egg):
        self.rec = pkm.Record(t, state, calc, bad_egg)
        self.obj = t.emu.alloc(b"\xdd" * 0xB8)
        t.emu.write(self.obj + 0x98, struct.pack("<Q", self.rec.obj))
        self.t = t
        r = self.rec.obj
        if t.rng.random() < 0.8:
            t.call(ACC_SET_EXP, r, t.rng.randrange(1300000))
        if t.rng.random() < 0.6:
            for setter in ACC_SET_EVS:
                t.call(setter, r, t.rng.choice([0, 4, 85, 252, t.rng.randrange(256)]))
        if t.rng.random() < 0.5:
            t.call(ACC_SET_HYPER, r, t.rng.choice([0, 0x3F, t.rng.getrandbits(8)]))
        if t.rng.random() < 0.2:
            t.call(ACC_SET_HP, r, 0)
        if t.rng.random() < 0.5:
            t.call(ACC_SET_LEVEL, r, t.rng.randrange(1, 101))
        if t.rng.random() < 0.7:
            for slot in range(4):
                move = t.rng.choice([0, 0, t.rng.randrange(1, 41), t.rng.randrange(1, 41), t.rng.choice(KYUREM_MOVES)])
                t.call(ACC_SET_MOVE, r, slot, move)
                t.call(ACC_SET_PP, r, slot, t.rng.randrange(64))
                t.call(ACC_SET_PP_UPS, r, slot, t.rng.randrange(5))

    def snapshot(self):
        return self.t.emu.read(self.obj, 0xB8) + self.rec.snapshot()


def mons(t, rounds):
    for _ in range(rounds):
        for state, calc, bad in pkm.STATES:
            yield Mon(t, state, calc, bad)


def str_buf(t, capacity):
    buf = t.emu.alloc(b"\xcc" * 0x40)
    obj = t.emu.alloc(0x80)
    t.emu.write(obj + 0x58, struct.pack("<Q", buf))
    t.emu.write(obj + 0x78, struct.pack("<HHB", capacity, 0, 1))
    return obj, buf


def test_wrappers(t):
    for m in mons(t, 2):
        for addr, mask in GETTERS:
            t.check((hex(addr), t.call(addr, m.obj) & mask, m.snapshot()))
        for addr in FLOAT_GETTERS:
            t.call(addr, m.obj)
            t.check((hex(addr), t.emu.vreg(0) & 0xFFFFFFFF))
        for addr, mask in SLOT_GETTERS:
            for slot in (0, 3, 4, 0x102):
                t.check((hex(addr), slot, t.call(addr, m.obj, slot) & mask))
        for ribbon in (0, 5, 40, 63, 64):
            t.check(t.call(HAS_RIBBON, m.obj, ribbon) & BOOL)
        for getter in (NICKNAME_BUF, OT_NAME_BUF):
            obj, buf = str_buf(t, 13)
            t.call(getter, m.obj, obj)
            t.check((hex(getter), t.emu.read(obj + 0x78, 6), t.emu.read(buf, 0x40)))
        for getter in (NICKNAME, OT_NAME):
            out = t.emu.alloc(b"\xcc" * 0x40)
            t.call(getter, m.obj, out, t.rng.choice([13, 4]))
            t.check((hex(getter), t.emu.read(out, 0x40)))
        out = t.emu.alloc(b"\xcc" * pkm.CORE_SIZE)
        t.call(SERIALIZE_CORE, m.obj, out)
        t.check(t.emu.read(out, pkm.CORE_SIZE))
        if m.rec.calc:            # the originals need party stats for these
            out = t.emu.alloc(b"\xcc" * (pkm.CORE_SIZE + pkm.CALC_SIZE))
            t.call(SERIALIZE, m.obj, out)
            t.check(t.emu.read(out, pkm.CORE_SIZE + pkm.CALC_SIZE))
        for addr, bits in SETTERS:
            t.call(addr, m.obj, t.rng.getrandbits(bits))
            t.check((hex(addr), m.snapshot()))
        t.call(SET_RIBBON, m.obj, t.rng.randrange(70))
        t.check(m.snapshot())
        record = t.emu.alloc(bytes(pkm.random_record(t.rng)[0]) + bytes(pkm.CALC_SIZE))
        t.call(DESERIALIZE_CORE, m.obj, record)
        t.check(m.snapshot())
        if m.rec.calc:
            t.call(DESERIALIZE, m.obj, record)
            t.check(m.snapshot())
        for addr in NO_ARGS:
            t.call(addr, m.obj)
            t.check((hex(addr), m.snapshot()))


def test_markings(t):
    for m in mons(t, 2):
        for mark in (-1, 0, 2, 5, 6, 7):
            t.check(t.call(HAS_MARKING, m.obj, mark & U32) & BOOL)
            if mark >= 0:
                t.check(t.call(GET_MARKING, m.obj, mark) & U16)
        for _ in range(4):
            t.call(SET_MARKING, m.obj, t.rng.randrange(-1, 8) & U32, t.rng.randrange(-1, 4) & U32)
            t.check(m.snapshot())
        t.call(CLEAR_MARKING, m.obj, t.rng.randrange(-1, 8) & U32)
        t.check(m.snapshot())
        marks = t.emu.alloc(struct.pack("<6i", *[t.rng.randrange(-1, 4) for _ in range(6)]))
        t.call(SET_ALL_MARKINGS, m.obj, marks)
        t.check(m.snapshot())
        out = t.emu.alloc(b"\xcc" * 24)
        t.call(GET_ALL_MARKINGS, m.obj, out)
        t.check(t.emu.read(out, 24))


def test_small_fields(t):
    for m in mons(t, 2):
        t.call(ADD_UNK_AE, m.obj, t.rng.choice([0, 1, 200, 255, 0x1FF]))
        t.check(m.snapshot())
        t.call(SET_UNK3C_LOW, m.obj, t.rng.getrandbits(16))
        t.call(SET_UNK3C_HIGH, m.obj, t.rng.getrandbits(16))
        t.check(m.snapshot())
        for field in range(20):
            t.check((field, t.call(GET_MET_INFO, m.obj, field) & U32))
            t.call(SET_MET_INFO, m.obj, field, t.rng.getrandbits(32))
            t.check((field, m.snapshot()))
        t.call(SET_MET_INFO, m.obj, 0xFFFFFFFF, 1)
        t.check(m.snapshot())


def test_ivs(t):
    for m in mons(t, 3):
        for stat in range(-1, 8):
            if stat >= 0:
                t.check(t.call(GET_IV, m.obj, stat) & U8)
            t.check(t.call(GET_EFFECTIVE_IV, m.obj, stat & U32) & U8)
            t.check(t.call(IS_HYPER_TRAINED, m.obj, stat & U32) & BOOL)
        t.check(t.call(HIDDEN_POWER, m.obj) & U8)
        for _ in range(3):
            t.call(SET_IV, m.obj, t.rng.randrange(8), t.rng.choice([0, 15, 31, 32, 0x100, t.rng.getrandbits(32)]))
            t.check(m.snapshot())


def test_evs(t):
    for m in mons(t, 3):
        for stat in range(8):
            t.check(t.call(GET_EV, m.obj, stat) & U8)
        t.check(t.call(GET_EV_TOTAL, m.obj) & U32)
        for _ in range(6):
            cur, val = t.rng.randrange(256), t.rng.choice([0, 100, 252, 300, t.rng.getrandbits(32)])
            t.check(t.call(CLAMP_EV, m.obj, cur, val) & U32)
        for fn in (SET_EV, ADD_EV, SUBTRACT_EV):
            for _ in range(2):
                t.call(fn, m.obj, t.rng.randrange(8), t.rng.choice([0, 10, 252, 600, t.rng.getrandbits(32)]))
                t.check((hex(fn), m.snapshot()))


def test_avs(t):
    for m in mons(t, 3):
        for stat in range(8):
            t.check(t.call(GET_AV, m.obj, stat) & U8)
        for _ in range(2):
            t.call(SET_AV, m.obj, t.rng.randrange(8), t.rng.choice([0, 199, 200, 0x1C7, t.rng.getrandbits(32)]))
            t.check(m.snapshot())
            t.call(ADD_AV, m.obj, t.rng.randrange(8), t.rng.choice([1, 100, 255, 0x1FF]))
            t.check(m.snapshot())
        t.call(SET_UNK_D7, m.obj, t.rng.choice([0, 99, 100, 101, 0x1FF]))
        t.check(m.snapshot())


def test_stats(t):
    for m in mons(t, 3):
        for stat in range(8):
            t.check((stat, t.call(GET_STAT, m.obj, stat) & U16, m.snapshot()))
        t.check((t.call(GET_MAX_HP, m.obj) & U16, t.call(GET_HP, m.obj) & U16))
        for fn in CALCS:
            t.check((hex(fn), t.call(fn, m.obj) & U16))
        t.check((t.call(IS_FULL_HP, m.obj) & BOOL, t.call(IS_FAINTED, m.obj) & BOOL, t.call(HAS_STATUS, m.obj) & BOOL))
        t.call(UPDATE_MAX_HP, m.obj, t.rng.randrange(2))
        t.check(m.snapshot())
        t.call(UPDATE_CP, m.obj)
        t.check(m.snapshot())
        t.call(RECALC_STATS, m.obj, t.rng.randrange(2))
        t.check(m.snapshot())
        t.call(SET_HP, m.obj, t.rng.choice([0, 1, 0xFFFF, t.rng.getrandbits(32)]))
        t.check(m.snapshot())
        t.call(RECOVER_HP, m.obj, t.rng.choice([0, 20, 0xFFFF, t.rng.getrandbits(32)]))
        t.check(m.snapshot())


def test_level_and_exp(t):
    for m in mons(t, 3):
        t.check((t.call(GET_LEVEL, m.obj) & U8, t.call(GET_LEVEL_EXP, m.obj) & U32,
                 t.call(GET_NEXT_LEVEL_EXP, m.obj) & U32, m.snapshot()))
        t.call(ADD_LEVELS, m.obj, t.rng.choice([0, 1, 5, 99, 0x101]))
        t.check(m.snapshot())
        t.call(SET_EXP, m.obj, t.rng.choice([0, 1000, 1000000, 2000000, t.rng.getrandbits(32)]))
        t.check(m.snapshot())
        t.call(ADD_EXP, m.obj, t.rng.choice([0, 500, 0xFFFFFFFF, t.rng.getrandbits(20)]))
        t.check(m.snapshot())


def some_move(t):
    return t.rng.choice([0, t.rng.randrange(1, 41), t.rng.randrange(1, 41), t.rng.choice(KYUREM_MOVES), 0x10005])


def test_moves(t):
    for m in mons(t, 2):
        for move in [0, 0x10000] + KYUREM_MOVES + list(range(1, 41, 3)):
            t.check(t.call(HAS_MOVE, m.obj, move) & BOOL)
        t.check(t.call(MOVE_COUNT, m.obj) & U8)
        for slot in (0, 1, 2, 3, 4, 0x101):
            t.check((t.call(GET_MAX_PP, m.obj, slot) & U8, t.call(GET_PP_UPS, m.obj, slot) & U8))
        t.check(t.call(CAN_LEARN_MACHINE, m.obj, t.rng.getrandbits(17)) & BOOL)
        for _ in range(3):
            slot = t.rng.choice([0, 1, 2, 3, 4, 0x103])
            t.call(SET_MOVE_PP, m.obj, slot, t.rng.choice([0, 5, 63, 0x1FF]))
            t.check(m.snapshot())
            t.call(RECOVER_PP, m.obj, t.rng.randrange(4), t.rng.choice([1, 30, 255, 0x110]))
            t.check(m.snapshot())
            t.call(SET_PP_UPS, m.obj, slot, t.rng.choice([0, 3, 4, 0x102]))
            t.check(m.snapshot())
            t.call(SET_MOVE, m.obj, t.rng.randrange(6), some_move(t))
            t.check(m.snapshot())
            t.call(SET_RELEARN_MOVE, m.obj, t.rng.randrange(6), some_move(t))
            t.check(m.snapshot())
            t.call(SWAP_MOVES, m.obj, t.rng.randrange(6), t.rng.randrange(6))
            t.check(m.snapshot())
            t.call(CLEAR_MOVE, m.obj, t.rng.randrange(6))
            t.check(m.snapshot())
        t.call(RECOVER_ALL_PP, m.obj)
        t.check(m.snapshot())
        t.call(COMPACT_MOVES, m.obj)
        t.check(m.snapshot())
        for fn in (LEARN_MOVE, PUSH_MOVE, LEARN_MOVE, PUSH_MOVE):
            t.check((hex(fn), t.call(fn, m.obj, some_move(t)) & U32, m.snapshot()))


def move_list(t):
    """A MoveList with a few moves (and leftovers past the count), or null."""
    if t.rng.random() < 0.3:
        return 0
    entries = [t.rng.randrange(1, 41) for _ in range(55)]
    count = t.rng.choice([0, 3, 54, 55, 60])
    return t.emu.alloc(struct.pack("<56I", *entries, count))


def test_move_list(t):
    for _ in range(40):
        seen = move_list(t) or t.emu.alloc(0xE0)
        for move in range(45):
            t.check(t.call(MOVE_LIST_CONTAINS, seen, move) & BOOL)
        t.call(MOVE_LIST_CLEAR, seen)
        t.check(t.emu.read(seen, 0xE0))


def test_learning(t):
    for m in mons(t, 3):
        for fn in (LEARN_LEVEL_UP_MOVE, LEARN_EVOLUTION_MOVE):
            index, move, seen = t.emu.alloc(struct.pack("<I", t.rng.choice([0, 0, 1, 3]))), t.emu.alloc(4), move_list(t)
            for _ in range(8):
                result = t.call(fn, m.obj, index, move, seen) & U32
                t.check((hex(fn), result, t.emu.read(index, 4), t.emu.read(move, 4),
                         t.emu.read(seen, 0xE0) if seen else b"", m.snapshot()))
                if result == 3:
                    break
        t.call(RESET_MOVES, m.obj)
        t.check(m.snapshot())
        for form in (0, 1, 2, 3):
            for slot in range(4):
                t.call(ACC_SET_MOVE, m.rec.obj, slot, t.rng.choice(KYUREM_MOVES + [0, 7]))
                t.call(ACC_SET_PP, m.rec.obj, slot, t.rng.randrange(64))
            t.call(UPDATE_KYUREM_MOVES, m.obj, form)
            t.check((form, m.snapshot()))


def name_buf(t, m):
    """A StrBuf holding the species' name in the record's language, or a random name."""
    if t.rng.random() < 0.5:
        species = t.call(0x71007247C0, m.obj) & U16
        language = t.call(0x7100724B60, m.obj) & U8
        text = struct.pack("<7H", *name_of(species, language))
    else:
        n = t.rng.randrange(0, 13)
        text = struct.pack("<%dH" % n, *[t.rng.randrange(0x20, 0x3000) for _ in range(n)]) + b"\0\0"
    obj, buf = str_buf(t, 13)
    t.emu.write(buf, text)
    return obj


def trainer(t, m):
    """A TrainerInfo: the Pokémon's original trainer, one that differs in one way, or a random one."""
    name = t.emu.alloc(0x40)
    t.call(OT_NAME, m.obj, name, 13)
    info = [t.call(GET_ID, m.obj) & U32, t.call(GET_OT_GENDER, m.obj) & U8, t.emu.read(name, 26)]
    kind = t.rng.randrange(5)
    if kind == 1:
        info[0] ^= 1 << t.rng.randrange(32)
    elif kind == 2:
        info[1] ^= 1
    elif kind == 3:
        info[2] = struct.pack("<13H", *[t.rng.randrange(0x20, 0x3000) for _ in range(12)], 0)
    return t.emu.alloc(struct.pack("<IBB", info[0], info[1], 0x5A) + info[2] + b"\0" * 8)


def test_names_and_trainers(t):
    for m in mons(t, 3):
        t.emu.write(SYSTEM[0] + 0x308, bytes([t.rng.randrange(12)]))
        t.check(t.call(IS_NICKNAME_SPECIES_NAME, m.obj) & BOOL)
        t.call(SET_NICKNAME, m.obj, name_buf(t, m))
        t.check(m.snapshot())
        t.check(t.call(IS_NICKNAME_SPECIES_NAME, m.obj) & BOOL)
        t.call(CLEAR_NICKNAME, m.obj)
        t.check((m.snapshot(), t.call(IS_NICKNAME_SPECIES_NAME, m.obj) & BOOL))
        t.call(SET_OT_NAME, m.obj, name_buf(t, m))
        t.check(m.snapshot())
        for _ in range(3):
            info = trainer(t, m)
            t.check(t.call(IS_ORIGINAL_TRAINER, m.obj, info) & BOOL)
            t.check((t.call(SET_CURRENT_TRAINER, m.obj, info) & BOOL, m.snapshot()))


def test_friendship(t):
    for m in mons(t, 3):
        for fn in (SET_FRIENDSHIP, ADD_FRIENDSHIP, SUBTRACT_FRIENDSHIP, SET_OT_FRIENDSHIP, SET_HANDLER_FRIENDSHIP):
            t.call(fn, m.obj, t.rng.choice([0, 1, 70, 255, 256, 0x1FF, t.rng.getrandbits(32)]))
            t.check((hex(fn), m.snapshot()))
        t.check(t.call(GET_HANDLER_FRIENDSHIP, m.obj) & U8)


def test_other_state(t):
    for nature in range(-1, 27):
        for stat in range(-1, 7):
            t.check(t.call(NATURE_MODIFIER, nature & U32, stat & U32) & U8)
    for m in mons(t, 3):
        t.call(SET_HYPER_TRAINED, m.obj, t.rng.randrange(-2, 8) & U32)
        t.check(m.snapshot())
        for flavor in range(6):
            t.check(t.call(NATURE_FLAVOR, m.obj, flavor) & U32)
        for check in range(4):
            t.check(t.call(IS_EGG, m.obj, check) & BOOL)
        t.check((t.call(IS_SHINY, m.obj) & BOOL, t.call(HAS_POKERUS, m.obj) & BOOL, t.call(HEIGHT_CLASS, m.obj) & U32))
        started = t.call(START_FAST_MODE, m.obj) & BOOL
        t.check((started, t.call(START_FAST_MODE, m.obj) & BOOL, m.snapshot()))
        t.check((t.call(END_FAST_MODE, m.obj, 0) & BOOL, m.snapshot()))
        t.check((t.call(END_FAST_MODE, m.obj, started) & BOOL, m.snapshot()))
        t.call(HEAL, m.obj)
        t.check(m.snapshot())


def test_size(t):
    floats = [0.0, 1.0, 0.5, 30.0, 123.25, 1e6, -4.0, float("inf")]
    for m in mons(t, 3):
        t.call(0x7100727370, m.obj, 0)         # CanLearnMachine: loads the species data
        t.call(UPDATE_ABSOLUTE_SIZE, m.obj)
        t.check(m.snapshot())
        for _ in range(3):
            height = t.rng.choice(floats + [t.rng.uniform(0, 500)])
            weight = t.rng.choice(floats + [t.rng.uniform(0, 5000)])
            bits = [struct.unpack("<I", struct.pack("<f", v))[0] for v in (height, weight)]
            t.call(SET_ABSOLUTE_SIZE, m.obj, fargs=bits)
            t.check(m.snapshot())
