"""decomp/src/battle/status_damage.cpp, condition_turn.cpp, battle_pokemon.cpp and calc.cpp's fraction of the
maximum HP against the originals: each condition's damage on made-up Pokémon (their conditions' 8 bytes made
up too), and the end of a turn for every condition, with made-up handlers on the events that change the
damage or stop it."""
import random
import struct

MAX_HP_FRACTION, CONDITION_DAMAGE, STATUS_DAMAGE, END_OF_TURN = (
    0x71009CFED0, 0x71009E40B0, 0x7100A07F90, 0x7100A147B0)
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, GET_VALUE, PP_OF_MOVE, SAVE, RESTORE, INDEX_OF, PUT, GET_POKEMON = (
    0x71009E2F30, 0x71009E3690, 0x71009E4A00, 0x7100A68E80, 0x7100A68EE0, 0x7100A6B7A0, 0x7100A11D70, 0x7100A095E0)
DAMAGE_MESSAGE, INDIRECT_DAMAGE, NEW_RECORD, PUSH_RECORD = 0x7100A150D0, 0x7100A002D0, 0x7100A0AFC0, 0x7100A0B020
TURNS = {0x7100A14450: "BindTurn", 0x7100A14210: "LeechSeedTurn", 0x7100A14140: "PerishSongTurn",
         0x7100A143A0: "IngrainTurn", 0x7100A14620: "AquaRingTurn"}

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def record(name, *args):
        STATE["t"].check((name,) + args)

    def get_id(e):
        record("GetId", e.reg(0))
        return pokemon(e).get("id", 0x1F)

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get(("value", e.reg(1) & U32), 0)

    def pp_of_move(e):
        record("GetPpOfMove", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("pp", 0)

    def save(e):
        record("Save", e.reg(0))
        return STATE["saved"]

    def restore(e):
        record("Restore", e.reg(0), e.reg(1) & U32)

    def index_of(e):
        record("IndexOf", e.reg(0), e.reg(1) & U8)
        return STATE["slot"]

    def put(e):
        record("Put", e.reg(0), e.reg(1) & U32, e.reg(2) & U32, e.reg(3) & U32)

    def damage_message(e):
        record("SetConditionDamageMessage", e.reg(0), e.reg(1), e.reg(2) & U32)
        return 0

    def indirect_damage(e):
        record("ApplyIndirectDamage", e.reg(0), e.reg(1), e.reg(2) & U32, e.reg(3) & U32, e.reg(4) & U32, e.reg(5),
               e.reg(6) & U8)
        return 1

    def new_record(e):
        record("NewRecord", e.reg(0), e.reg(1) & U32, e.reg(2) & U8)
        return STATE["record"]

    def push_record(e):
        record("PushRecord", e.reg(0), e.reg(1), e.read(e.reg(1), 0x18))

    def turn(name):
        def run(e):
            record(name, e.reg(0), e.reg(1))
        return run
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(PP_OF_MOVE, "GetPpOfMove", pp_of_move)
    emu.stub_function(SAVE, "Save", save)
    emu.stub_function(RESTORE, "Restore", restore)
    emu.stub_function(INDEX_OF, "IndexOf", index_of)
    emu.stub_function(PUT, "Put", put)
    emu.stub_function(DAMAGE_MESSAGE, "SetConditionDamageMessage", damage_message)
    emu.stub_function(INDIRECT_DAMAGE, "ApplyIndirectDamage", indirect_damage)
    emu.stub_function(NEW_RECORD, "NewRecord", new_record)
    emu.stub_function(PUSH_RECORD, "PushRecord", push_record)
    for addr, name in TURNS.items():
        emu.stub_function(addr, name, turn(name))
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def make_pokemon(t, r):
    """A battle Pokémon with made-up conditions (8 bytes each from +0x98) and turn counters (+0x1e8)."""
    p = t.emu.alloc(0x1000)
    data = bytearray(0x400)
    for c in range(0x2A):
        kind = r.choice([0, 0, 1, 2, 3, 4, 5, 6, 7])
        entry = r.getrandbits(64) & ~7 | kind
        if c == 5 and r.random() < 0.5:
            entry = entry & ~0x3F07 | 0xF01                 # bad poison
        struct.pack_into("<Q", data, 0x98 + 8 * c, entry)
        data[0x1E8 + c] = r.choice([0, 1, 2, 5, 15, 16, 0xFF])
    t.emu.write(p, bytes(data))
    STATE["pokemon"][p] = {
        "id": r.choice([0, 1, 5, 0x101]),
        ("value", 0xD): r.choice([0, 1, 100]),
        ("value", 0xE): r.choice([0, 1, 7, 15, 16, 17, 100, 255, 0xFFFF, 0x10000, r.getrandbits(32)]),
        ("value", 0x11): r.choice([0, 0, 213, 213, 99, 0x1D5]),
        "pp": r.choice([0, 0, 1, 0x10000, 5]),
    }
    return p


def test_max_hp_fraction(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    STATE["pokemon"] = {}
    for _ in range(300):
        p = make_pokemon(t, r)
        for divisor in (1, 2, 3, 4, 8, 16, 17, 0x10000, U32):
            t.check(t.call(MAX_HP_FRACTION, p, divisor) & U32)


def test_condition_damage(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    STATE["pokemon"] = {}
    for _ in range(400):
        p = make_pokemon(t, r)
        for condition in range(0x2E):
            t.check(t.call(CONDITION_DAMAGE, p, condition) & U32)


class Battle:
    """A made-up battle: a flow object, a Pokémon, and handlers on the damage's events that change their
    variables in memory the way RewriteVar would. The handlers draw from a generator of their own."""

    EVENTS = [0x82, 0x98]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in (0x2900, 0x100, 0x200, 0x40, 0x300, 0x20)]
        self.flow, setup_obj, mons, commands, field, STATE["record"] = e.scene
        for addr, n in zip(e.scene, (0x2900, 0x100, 0x200, 0x40, 0x300, 0x20)):
            e.write(addr, bytes(r.getrandbits(8) for _ in range(n)) if n == 0x20 else bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(self.flow + 0x2720, bytes(r.getrandbits(8) for _ in range(0x24)))     # the message, cleared
        STATE["saved"] = r.getrandbits(32)
        STATE["slot"] = r.choice([0, 1, 3, 4, 4, 0x104])
        self.mon = make_pokemon(t, r)
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
            seen = tuple(self.full(l) for l in (2, 4, 0x21, 0x38, 0x5B, 0x6A))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x82:
                self.write(0x38, r.choice([0, 0, 1, 12, 0x10000, -1]))
            else:
                self.write(0x5B, r.choice([0, 1, 0x100]))
        return run


def test_status_damage(t):
    r = t.rng
    for _ in range(600):
        b = Battle(t)
        condition = r.choice([4, 5, 9, 10, 4, 5, 9, 10, 0, 3, 6, 8, 11, 0x24, r.randrange(0x2E)])
        t.call(STATUS_DAMAGE, b.flow, b.mon, condition, r.choice([1, 6, 12, 100, 0xFFFF, r.getrandbits(32)]))
        t.check(t.emu.read(b.flow + 0x2720, 0x24))
        t.check(t.emu.read(VARS, 0x5A4))


def test_end_of_turn(t):
    r = t.rng
    for _ in range(600):
        b = Battle(t)
        condition = r.choice([4, 5, 9, 10, 8, 0x12, 0x14, 0x15, 0x17, 0x17, 0x24, r.randrange(0x2E)])
        t.call(END_OF_TURN, b.flow, b.mon, condition)
        t.check(t.emu.read(b.flow + 0x2720, 0x24))
        t.check(t.emu.read(STATE["record"], 0x20))
        t.check(t.emu.read(VARS, 0x5A4))
