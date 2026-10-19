"""decomp/src/battle/before_move.cpp and calc.cpp's rolls against the originals: the percentage and hit-count
rolls on every roll, and whether made-up Pokémon can use their moves (sleep, freezing, disobedience, PP,
flinching, the conditions that stop a move, confusion, paralysis, love), with made-up handlers on the events
that let moves and abilities change that."""
import random
import struct

ROLL_PERCENT, ROLL_HIT_COUNT = 0x71009D04B0, 0x71009D03D0
FAIL_REASON, CURE_STATUS, COUNT_SLEEP, CHECK_THAW, FAIL_EVENT, CONFUSION, IN_LOVE = (
    0x71009FF990, 0x71009FFDF0, 0x71009FFEB0, 0x71009FFF60, 0x7100A00030, 0x7100A00190, 0x7100A00420)
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, GET_STATUS, HAS_CONDITION, CONDITION_PARAM, HAS_FLAG, GET_CONDITION = (
    0x71009E2F30, 0x71009E3F70, 0x71009E3DA0, 0x71009E3FE0, 0x71009E5390, 0x71009E4020)
COUNT_SLEEP_TURN, COUNT_CONFUSION_TURN, BPP_CURE_STATUS, CURE_CONDITION = (
    0x71009E5F50, 0x71009E6080, 0x71009E5D70, 0x71009E5FF0)
FIND_MOVE, GET_PP, LOCKED_MOVE, CHOICE_MOVE, CONDITION_VALUE = (
    0x71009E4DC0, 0x71009E49D0, 0x71009E6E10, 0x71009E4180, 0x7100A15440)
MOVE_HAS_FLAG, MOVE_HAS_POWER, MOVE_CATEGORY = 0x710073C7D0, 0x710073CCE0, 0x710073C9C0
FIELD_HAS_EFFECT, FIELD_IMPRISONED, SET_COUNT, SET_GET, UNK1798_INDEX = (
    0x7100A685A0, 0x7100A687B0, 0x7100A6A6C0, 0x7100A6A560, 0x7100A6B7A0)
PUT, PUT_MESSAGE, NEXT, GET_POKEMON = 0x7100A11D70, 0x7100A12B40, 0x71009E89F0, 0x7100A095E0

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def record(name, *args):
        STATE["t"].check((name,) + args)

    def simple(name, key, mask=None, nargs=1):
        def get(e):
            args = tuple(e.reg(i) & (U32 if i else ~0) for i in range(nargs))
            record(name, *args)
            v = pokemon(e).get(key, 0)
            return v(e) if callable(v) else v
        return get

    def has(name, key):
        def get(e):
            record(name, e.reg(0), e.reg(1) & U32)
            return 1 if (e.reg(1) & U32) in pokemon(e).get(key, ()) else 0
        return get

    def condition(e):
        record("GetCondition", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("entries", {}).get(e.reg(1) & U32, 0)

    def condition_value(e):
        entry = struct.unpack("<Q", e.read(e.reg(0), 8))[0]
        record("ConditionValue", entry)
        return STATE["values"].get(entry, 0)

    def find_move(e):
        record("FindMove", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("slot", 4)

    def get_pp(e):
        record("GetPp", e.reg(0), e.reg(1) & U8)
        return pokemon(e).get("pp", 0)

    def move_flag(e):
        record("MoveHasFlag", e.reg(0) & U32, e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in STATE["moveFlags"] else 0

    def move_value(name):
        def get(e):
            record(name, e.reg(0) & U32)
            return STATE[name]
        return get

    def field_effect(e):
        record("FieldHasEffect", e.reg(0), e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in STATE["field"] else 0

    def imprisoned(e):
        record("IsImprisoned", e.reg(0), e.reg(1), e.reg(2), e.reg(3), e.reg(4) & U32)
        return STATE["imprisoned"]

    def set_count(e):
        record("SetCount", e.reg(0))
        return len(STATE["targets"])

    def set_get(e):
        record("SetGet", e.reg(0), e.reg(1) & U32)
        i = e.reg(1) & U32
        return STATE["targets"][i] if i < len(STATE["targets"]) else 0

    def index_of(e):
        record("IndexOf", e.reg(0), e.reg(1) & U8)
        return STATE["entry"]

    def put(e):                                      # only the arguments each command has
        count = {0x12: 1, 0x13: 2, 0x4B: 2, 0x65: 2}.get(e.reg(1) & U32, 5)
        record("Put", e.reg(0), *(e.reg(i) & U32 for i in range(1, 2 + count)))

    def put_message(e):                              # up to the 0xffff0000 that ends them
        args = []
        for i in range(1, 8):
            args.append(e.reg(i) & U32)
            if args[-1] == 0xFFFF0000:
                break
        record("PutMessage", e.reg(0), *args)

    def nxt(e):
        n = e.reg(1)
        record("Next", e.reg(0), n)
        if STATE.get("roll") is not None:
            return STATE["roll"]
        if n == 100:                                # near the thresholds the rolls are compared with
            return STATE["t"].rng.choice([0, 19, 20, 24, 25, 32, 33, 34, 49, 50, 69, 70, 84, 85, 99,
                                          STATE["t"].rng.randrange(100)])
        return STATE["t"].rng.randrange(n) if n else 0
    emu.stub_function(GET_ID, "GetId", simple("GetId", "id"))
    emu.stub_function(GET_STATUS, "GetStatus", simple("GetStatus", "status"))
    emu.stub_function(HAS_CONDITION, "HasCondition", has("HasCondition", "conditions"))
    emu.stub_function(CONDITION_PARAM, "GetConditionParam", simple("GetConditionParam", "loves", nargs=2))
    emu.stub_function(HAS_FLAG, "HasFlag", has("HasFlag", "flags"))
    emu.stub_function(GET_CONDITION, "GetCondition", condition)
    emu.stub_function(COUNT_SLEEP_TURN, "CountSleepTurn", simple("CountSleepTurn", "wakes"))
    emu.stub_function(COUNT_CONFUSION_TURN, "CountConfusionTurn", simple("CountConfusionTurn", "snapsOut"))
    emu.stub_function(BPP_CURE_STATUS, "BattlePokemon::CureStatus", simple("BattlePokemon::CureStatus", None))
    emu.stub_function(CURE_CONDITION, "CureCondition", simple("CureCondition", None, nargs=2))
    emu.stub_function(FIND_MOVE, "FindMove", find_move)
    emu.stub_function(GET_PP, "GetPp", get_pp)
    emu.stub_function(LOCKED_MOVE, "GetLockedMove", simple("GetLockedMove", "locked"))
    emu.stub_function(CHOICE_MOVE, "GetChoiceMove", simple("GetChoiceMove", "choice"))
    emu.stub_function(CONDITION_VALUE, "ConditionValue", condition_value)
    emu.stub_function(MOVE_HAS_FLAG, "MoveHasFlag", move_flag)
    emu.stub_function(MOVE_HAS_POWER, "HasPower", move_value("HasPower"))
    emu.stub_function(MOVE_CATEGORY, "GetCategory", move_value("GetCategory"))
    emu.stub_function(FIELD_HAS_EFFECT, "FieldHasEffect", field_effect)
    emu.stub_function(FIELD_IMPRISONED, "IsImprisoned", imprisoned)
    emu.stub_function(SET_COUNT, "SetCount", set_count)
    emu.stub_function(SET_GET, "SetGet", set_get)
    emu.stub_function(UNK1798_INDEX, "IndexOf", index_of)
    emu.stub_function(PUT, "Put", put)
    emu.stub_function(PUT_MESSAGE, "PutMessage", put_message)
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_move_fail_rolls(t):
    STATE.clear()
    STATE["t"] = t
    for roll in range(100):
        STATE["roll"] = roll
        for chance in (0, 1, 20, 25, 33, 50, 99, 100, 101, U32):
            t.check(t.call(ROLL_PERCENT, chance) & U8)
        for hits in (0, 1, 2, 3, 4, 5, 6, 0x105, 0x205):
            t.check(t.call(ROLL_HIT_COUNT, hits) & U8)
    STATE["roll"] = None


class Battle:
    """A made-up battle: a flow object, a Pokémon about to move, its move, and handlers on the events that
    change their variables in memory the way RewriteVar would. The handlers draw from a generator of their
    own."""

    EVENTS = [0x22, 0x23, 0xD4]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in (0x2900, 0x500, 0x100, 0x200, 0x40, 0x300)]
        self.flow, params, setup_obj, mons, commands, field = e.scene
        for addr, n in zip(e.scene, (0x2900, 0x500, 0x100, 0x200, 0x40, 0x300)):
            e.write(addr, bytes(n))
        e.write(params + 0x4A1, bytes([r.choice([0, 0, 0, 0, 0, 1, 0x80])]))
        e.write(setup_obj + 0x80, struct.pack("<Q", params))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(self.flow + 0x2804, struct.pack("<I", r.choice([0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 0x104])))
        self.move_id = r.choice([0xA5, 0x108, 33, 85, 0x108, r.getrandbits(16)])
        other = r.choice([1, 2, 0x10A5, r.getrandbits(16)])
        STATE["moveFlags"] = set(f for f in (8, 9, 10, 0xC, 0xE) if r.random() < 0.3)
        STATE["HasPower"] = r.choice([0, 1, 1])
        STATE["GetCategory"] = r.choice([0, 1, 2, r.getrandbits(32)])
        STATE["field"] = set(f for f in (2, 3, 7) if r.random() < 0.25)
        STATE["imprisoned"] = r.choice([0, 1])
        STATE["entry"] = r.choice([0, 1, 3, 4, 4, 0x104])
        STATE["values"] = {}
        entries = {}
        for cond in (0xD, 0x17):
            entry = r.getrandbits(64)
            entries[cond] = entry
            STATE["values"][entry] = r.choice([self.move_id, self.move_id, 0xA5, other, 0x10000 | self.move_id])
        self.mon = e.alloc(0x400)
        STATE["targets"] = [e.alloc(0x400) for _ in range(r.choice([0, 1, 2, 3]))]
        STATE["pokemon"] = {self.mon: {
            "id": r.choice([0, 1, 5, 0x101]),
            "status": r.choice([0, 0, 0, 1, 1, 1, 2, 3, 4, 5]),
            "conditions": set(c for c in (6, 7, 0xB, 0xD, 0xF, 0x13, 0x17, 0x18, 0x1A, 0x1C) if r.random() < 0.2),
            "flags": set(f for f in (4, 6) if r.random() < 0.15),
            "entries": entries,
            "wakes": r.choice([0, 1]),
            "snapsOut": r.choice([0, 1]),
            "slot": r.choice([0, 1, 3, 4, 0x104]),
            "pp": r.choice([0, 0, 1, 5, 0x100, 0x10000]),
            "locked": r.choice([self.move_id, self.move_id, other, 0xA5]),
            "choice": r.choice([self.move_id, self.move_id, other, 0xA5, 0x10000 | self.move_id]),
            "loves": r.choice([0, 1, 0x1234, 0x12345]),
        }}
        for i, p in enumerate(STATE["targets"]):
            STATE["pokemon"][p] = {"id": i + 2}
        t.call(RESET)
        STATE["battle"] = self
        if not hasattr(e, "handlers"):              # made once per test: the emulator has room for few
            e.handlers = {ev: e.host_function("on%x" % ev, Battle.handler(ev)) for ev in self.EVENTS}
            e.tables = {ev: e.alloc(struct.pack("<IIQ", ev, 0, e.handlers[ev])) for ev in self.EVENTS}
        for ev in self.EVENTS:
            if r.random() < 0.6:
                table = e.tables[ev]
                t.call(ADD, r.choice([0, 1, 2, 3, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8),
                       r.choice([0, 1, 2]), table, 1)

    def move(self):
        r = self.r
        data = bytearray(r.getrandbits(8) for _ in range(0x28))
        struct.pack_into("<I", data, 0, self.move_id)
        struct.pack_into("<I", data, 4, r.getrandbits(16))
        struct.pack_into("<I", data, 0xC, r.choice([0, 1, 2]))
        struct.pack_into("<I", data, 0x20, r.choice([0, 0, 0, 1, 0x100]))
        return self.e.alloc(bytes(data))

    def targets(self):
        return self.e.alloc(0x40) if STATE["targets"] or self.r.random() < 0.5 else 0

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
            seen = tuple(self.full(l) for l in (2, 3, 5, 6, 7, 8, 9, 0x12, 0x17, 0x19, 0x1C, 0x1E, 0x28, 0x4A))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x23:
                self.write(0x28, r.choice([0, 0, 1, 0xF, 0x13, 0x11, 0x100]))
            else:
                self.write(0x4A, r.choice([0, 1, 0x100]))
        return run


def test_move_fail_helpers(t):
    r = t.rng
    for _ in range(300):
        b = Battle(t)
        mon, move = b.mon, b.move()
        which = r.randrange(6)
        if which == 0:
            t.call(CURE_STATUS, b.flow, mon, r.choice([0, 1, 2, 3, 4, 0x102]), r.choice([0, 1, 0x100, 0x101]))
        elif which == 1:
            t.call(COUNT_SLEEP, b.flow, mon)
        elif which == 2:
            t.check(t.call(CHECK_THAW, b.flow, mon, r.choice([b.move_id, r.getrandbits(32)])) & U8)
        elif which == 3:
            t.check(t.call(FAIL_EVENT, b.flow, mon, b.move_id, r.choice([0x23, 0x22, 0x10]),
                           r.choice([move, move, 0]), b.targets()) & U32)
        elif which == 4:
            t.check(t.call(CONFUSION, b.flow, mon) & U8)
        else:
            t.call(IN_LOVE, b.flow, mon)
        t.check(t.emu.read(VARS, 0x5A4))


def test_move_fail_reason(t):
    r = t.rng
    for _ in range(2000):
        b = Battle(t)
        flags = t.emu.alloc(bytes([r.choice([0, 0, 8, 0xFF, 0xF7])]))
        t.check(t.call(FAIL_REASON, b.flow, b.mon, b.move(), r.choice([0, 0, 1, 0x100, 0x101]), b.targets(),
                       flags) & U32)
        t.check(t.emu.read(VARS, 0x5A4))
