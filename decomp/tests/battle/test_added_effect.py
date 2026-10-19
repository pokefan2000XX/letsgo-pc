"""decomp/src/battle/added_effect.cpp and calc.cpp's condition maker against the originals: a condition's 8
bytes for every duration a move can give, and the chances of the added effects (a condition, a flinch, a stat
change) on made-up moves and Pokémon, with made-up handlers on the events that abilities and items answer."""
import random
import struct

from unicorn.arm64_const import UC_ARM64_REG_SP

MAKE_CONDITION = 0x71009D0290
FLINCH_CHANCE, ROLL_FLINCH, MOVE_CONDITION, ADDED_EFFECT, STAT_CHANGE = (
    0x7100A038A0, 0x7100A03A50, 0x7100A04B50, 0x7100A05600, 0x7100A05720)
APPLY_FLINCH, APPLY_ADDED_EFFECT = 0x7100A03990, 0x7100A03240
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, HAS_RULE, NEXT, GET_POKEMON = 0x71009E2F30, 0x71009DA1F0, 0x71009E89F0, 0x7100A095E0
MOVE_GETTERS = {0x710073CC70: "GetFlinchChance", 0x710073CE20: "GetInflictCondition",
                0x710073CE90: "GetInflictChance", 0x710073CF00: "GetInflictDuration"}
STAT_CHANCE, STAT_CHANGE_DATA = 0x710073D200, 0x710073D110
EFFECT_KIND, HAS_FLAG, SET_FLAG, IS_FAINTED = 0x710073CA30, 0x71009E5390, 0x71009E5C40, 0x71009E3970
SAVE, RESTORE, GIVE_CONDITION, APPLY_STAT_CHANGES = 0x7100A68E80, 0x7100A68EE0, 0x7100A04CF0, 0x71009FE040

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STACK_HIGH, STACK_END = 0x73, 0x110000           # a pointer to the stack in two variables: 0x73, then the rest
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def get_id(e):
        record("GetId", e.reg(0))
        return STATE["pokemon"].get(e.reg(0), {}).get("id", 0x1F)

    def rule(e):
        record("HasRule", e.reg(0), e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in STATE["rules"] else 0

    def move_value(name):
        def get(e):
            record(name, e.reg(0) & U32)
            return STATE["move"][name]
        return get

    def stat_chance(e):
        record("GetStatChangeChance", e.reg(0) & U32, e.reg(1) & U32)
        return STATE["move"]["statChance"]

    def stat_change(e):
        record("GetStatChange", e.reg(0) & U32, e.reg(1) & U32)
        stat, stages = STATE["move"]["statChange"]
        e.write(e.reg(2), struct.pack("<i", stages))
        return stat & U32

    def nxt(e):
        n = e.reg(1)
        record("Next", e.reg(0), n)
        if n == 100:                                # near the thresholds the rolls are compared with
            return STATE["t"].rng.choice([0, 1, 9, 10, 19, 20, 29, 30, 49, 50, 99, STATE["t"].rng.randrange(100)])
        return STATE["t"].rng.randrange(n) if n else 0
    def pokemon_value(name, key):
        def get(e):
            record(name, e.reg(0), e.reg(1) & U32)
            p = STATE["pokemon"].get(e.reg(0), {})
            return 1 if (e.reg(1) & U32) in p.get(key, ()) else 0
        return get

    def fainted(e):
        record("IsFainted", e.reg(0))
        return STATE["pokemon"].get(e.reg(0), {}).get("fainted", 0)

    def set_flag(e):
        record("SetFlag", e.reg(0), e.reg(1) & U32)

    def save(e):
        record("Save", e.reg(0))
        return STATE["saved"]

    def restore(e):
        record("Restore", e.reg(0), e.reg(1) & U32)

    def give_condition(e):                           # x3 isn't passed
        sp = e.uc.reg_read(UC_ARM64_REG_SP)
        record("GiveMoveCondition", e.reg(0), e.reg(1), e.reg(2), e.reg(4) & U32, e.reg(5), e.reg(6) & U32,
               e.reg(7) & U32, e.read(sp, 1)[0], e.read(sp + 8, 1)[0])
        return 1

    def apply_stat_changes(e):
        record("ApplyMoveStatChanges", e.reg(0), e.reg(1), e.reg(2), e.reg(3), e.reg(4) & U32, e.reg(5) & U8,
               e.reg(6) & U8)
    emu.stub_function(EFFECT_KIND, "GetEffectKind", move_value("GetEffectKind"))
    emu.stub_function(HAS_FLAG, "HasFlag", pokemon_value("HasFlag", "flags"))
    emu.stub_function(SET_FLAG, "SetFlag", set_flag)
    emu.stub_function(IS_FAINTED, "IsFainted", fainted)
    emu.stub_function(SAVE, "Save", save)
    emu.stub_function(RESTORE, "Restore", restore)
    emu.stub_function(GIVE_CONDITION, "GiveMoveCondition", give_condition)
    emu.stub_function(APPLY_STAT_CHANGES, "ApplyMoveStatChanges", apply_stat_changes)
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(HAS_RULE, "HasRule", rule)
    for addr, name in MOVE_GETTERS.items():
        emu.stub_function(addr, name, move_value(name))
    emu.stub_function(STAT_CHANCE, "GetStatChangeChance", stat_chance)
    emu.stub_function(STAT_CHANGE_DATA, "GetStatChange", stat_change)
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_make_condition_data(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    p = t.emu.alloc(0x400)
    STATE["pokemon"] = {p: {"id": 5}}
    out = t.emu.alloc(8)
    for _ in range(3000):
        STATE["pokemon"][p]["id"] = r.choice([0, 1, 5, 0x1F, 0x25, 0x101])
        duration = r.randrange(16) | r.randrange(64) << 4 | r.randrange(64) << 10
        t.emu.write(out, struct.pack("<Q", 0x1122334455667788))
        t.call(MAKE_CONDITION, duration | r.choice([0, 0, 0x10000, 0xFFFF0000]), p, out)
        t.check(t.emu.read(out, 8))


class Battle:
    """A made-up battle: a flow object, two Pokémon, a move, and handlers on the added effects' events that
    change their variables in memory the way RewriteVar would. The handlers draw from a generator of their
    own."""

    EVENTS = [0x67, 0x70, 0x7B, 0x83, 0x84, 0x85]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400, 0x40)]
        self.flow, setup_obj, mons, commands, field, self.attacker, self.defender, self.move = e.scene
        for addr, n in zip(e.scene, (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400, 0x40)):
            e.write(addr, bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        self.move_id = r.choice([33, 85, 0x1234, r.getrandbits(32)])
        e.write(self.move, struct.pack("<I", self.move_id))
        STATE["rules"] = set(x for x in (0, 0xF) if r.random() < 0.15)
        STATE["move"] = {
            "GetFlinchChance": r.choice([0, 0, 10, 20, 30, 100]),
            "GetInflictCondition": r.choice([0, 1, 2, 3, 4, 5, 6, 0xFFFF, 0x10]),
            "GetInflictChance": r.choice([0, 10, 20, 30, 100, 0x100, 0xFFFFFFFF]),
            # kinds 1 to 4 only: for others the original copies whatever is on its stack
            "GetInflictDuration": r.choice([1, 2, 3, 4]) | r.randrange(64) << 4 | r.randrange(64) << 10
            | r.choice([0, 0x10000]),
            "statChance": r.choice([0, 10, 30, 100, 0x10A]),
            "statChange": (r.choice([0, 1, 2, 5, 8, 8, 9]), r.choice([0, 1, -1, 2, -2, 3])),
            "GetEffectKind": r.choice([0, 4, 4, 6, 6, 7, 0x104]),
        }
        STATE["saved"] = r.getrandbits(32)
        e.write(self.flow + 0x1500, struct.pack("<I", r.choice([0, 1, 5, 0xFFFFFFFE, 0xFFFFFFFF])))
        for i, p in enumerate((self.attacker, self.defender)):
            STATE["pokemon"][p] = {"id": r.choice([i, i, 5, 0x101]), "fainted": 1 if r.random() < 0.2 else 0,
                                   "flags": set(f for f in (4, 5, 6) if r.random() < 0.2)}
        t.call(RESET)
        STATE["battle"] = self
        if not hasattr(e, "handlers"):              # made once per test: the emulator has room for few
            e.handlers = {ev: e.host_function("on%x" % ev, Battle.handler(ev)) for ev in self.EVENTS}
            e.tables = {ev: e.alloc(struct.pack("<IIQ", ev, 0, e.handlers[ev])) for ev in self.EVENTS}
        for ev in self.EVENTS:
            if r.random() < 0.6:
                t.call(ADD, r.choice([0, 1, 2, 3, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8),
                       r.choice([0, 1, 2]), e.tables[ev], 1)

    def slot(self, label):
        frame = struct.unpack("<I", self.e.read(VARS, 4))[0]
        for i in range(frame, 96):
            lab = struct.unpack("<H", self.e.read(LABELS + 2 * i, 2))[0]
            if lab == 0:
                return None
            if lab == label:
                return i
        return None

    def write(self, label, value):
        i = self.slot(label)
        if i is not None:
            self.e.write(VALUES + 4 * i, struct.pack("<i", value))

    def full(self, label):
        """A variable's value, mode, minimum and maximum."""
        i = self.slot(label)
        if i is None:
            return None
        return (struct.unpack("<i", self.e.read(VALUES + 4 * i, 4))[0], self.e.read(MODES + i, 1)[0],
                struct.unpack("<i", self.e.read(MINS + 4 * i, 4))[0], struct.unpack("<i", self.e.read(MAXES + 4 * i, 4))[0])

    @staticmethod
    def handler(event):
        def run(e):
            self = STATE["battle"]
            r = self.hr
            seen = tuple(self.full(l) for l in (3, 4, 0x12, 0x21, 0x25, 0x26, 0x2B, 0x2C, 0x3B, 0x4A, 0x4E))
            high, low = self.full(0x23), self.full(0x24)        # a pointer to the condition's 8 bytes
            pointer = None if high is None else (high[0] & U32) << 32 | (low[0] & U32)
            data = None if pointer is None else self.e.read(pointer, 8)
            self.t.check(("event", event, e.reg(2) & U8, data) + seen)
            if r.random() < 0.3:
                return
            if event == 0x7B:
                self.write(0x21, r.choice([0, 1, 4, 0xFFFF, 0x10000]))
                self.write(0x2C, r.choice([0, 1, 20, 60, 0x100, -5]))
                self.write(0x4A, r.choice([0, 0, 1, 0x100]))
                if pointer is not None and r.random() < 0.5:
                    self.e.write(pointer, struct.pack("<Q", r.getrandbits(64)))
            elif event == 0x83:
                self.write(0x2C, r.choice([0, 10, 30, -1]))
                self.write(0x4A, r.choice([0, 1]))
                self.write(0x4E, r.choice([0, 1, 0x100]))
            elif event == 0x84:
                self.write(0x2C, r.choice([0, 10, 30, 0x105, -1]))
                self.write(0x4A, r.choice([0, 1]))
            elif event == 0x67:
                self.write(0x2C, r.choice([0, 10, 30, 60, 0x105]))
                self.write(0x4A, r.choice([0, 1, 0x100]))
            else:
                self.write(0x25, r.choice([0, 1, 8, 8, 3]))
                self.write(0x26, r.choice([0, 1, -1, 2, 6]))
                self.write(0x3B, r.choice([0, 1, 2, 3, 0x102]))
        return run


def variables(t):
    """The variables after a call, without a pointer to the stack (which differs between the two runs)."""
    data = bytearray(t.emu.read(VARS, 0x5A4))
    values = list(struct.unpack_from("<96I", data, 0xC4))
    for i in range(95):
        if values[i] == STACK_HIGH and values[i + 1] < STACK_END:
            struct.pack_into("<II", data, 0xC4 + 4 * i, 0, 0)
    return bytes(data)


def test_added_effects(t):
    r = t.rng
    for _ in range(2500):
        b = Battle(t)
        attacker, defender = (b.attacker, b.defender) if r.random() < 0.8 else (b.defender, b.attacker)
        which = r.randrange(5)
        if which == 0:
            t.check(t.call(FLINCH_CHANCE, b.flow, b.move_id, attacker) & U32)
        elif which == 1:
            t.check(t.call(ROLL_FLINCH, b.flow, defender, r.choice([0, 10, 20, 30, 0x10A, U32])) & U8)
        elif which == 2:
            out = t.emu.alloc(struct.pack("<Q", 0x1122334455667788))
            t.check(t.call(MOVE_CONDITION, b.flow, b.move_id, attacker, defender, out) & U32)
            t.check(t.emu.read(out, 8))
        elif which == 3:
            t.check(t.call(ADDED_EFFECT, b.flow, b.move, attacker, defender) & U8)
        else:
            stat, stages = t.emu.alloc(8), t.emu.alloc(8)
            t.call(STAT_CHANGE, b.flow, b.move_id, r.choice([0, 1, 2, 3]), attacker, defender, stat, stages)
            t.check((t.emu.read(stat, 4), t.emu.read(stages, 4)))
        t.check(variables(t))


def test_apply_added_effects(t):
    r = t.rng
    for _ in range(1500):
        b = Battle(t)
        attacker, defender = (b.attacker, b.defender) if r.random() < 0.8 else (b.defender, b.attacker)
        if r.random() < 0.5:
            t.check(t.call(APPLY_FLINCH, b.flow, defender, r.choice([0, 10, 30, 99, 100, 100, 0x10A, U32])) & U8)
        else:
            t.call(APPLY_ADDED_EFFECT, b.flow, b.move, attacker, defender, r.getrandbits(32), r.choice([0, 0, 1]))
        t.check(t.emu.read(b.flow + 0x1500, 4))
        t.check(variables(t))
