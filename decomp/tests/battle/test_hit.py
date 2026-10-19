"""decomp/src/battle/hit.cpp and calc.cpp's accuracy and critical arithmetic against the originals: the
accuracy stages and the critical roll on edge values, and whether moves hit and land critical hits on made-up
Pokémon, with made-up handlers changing the events' variables as moves, abilities and items do."""
import random
import struct

ACCURACY_AFTER_STAGES, ROLL_CRITICAL = 0x71009CFF00, 0x71009CFF40
ALWAYS_HITS, CHECK_HIT, SKIPS_CHECK, CALC_ACCURACY, CHECK_CRITICAL, ONE_HIT_KO = (
    0x71009FD1E0, 0x71009FE7A0, 0x71009FE9F0, 0x71009FEA90, 0x7100A09250, 0x7100A05DE0)
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
HAS_RULE, GET_FRIENDSHIP, GET_ID, GET_VALUE, HAS_CONDITION, CONDITION_PARAM, HAS_FLAG, CRITICAL_STAGE, NEXT = (
    0x71009DA1F0, 0x71009DAD10, 0x71009E2F30, 0x71009E3690, 0x71009E3DA0, 0x71009E3FE0, 0x71009E5390, 0x71009E5B00,
    0x71009E89F0)
UNK1798_CHECK, GET_POKEMON = 0x7100A6B820, 0x7100A095E0
MOVE_ACCURACY, NEVER_MISSES, MOVE_CRITICAL_STAGE = 0x710073CB10, 0x710073CB80, 0x710073CD60

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

    def has_condition(e):
        record("HasCondition", e.reg(0), e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in pokemon(e).get("conditions", ()) else 0

    def condition_param(e):
        record("GetConditionParam", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("lockOn", 0)

    def has_flag(e):
        record("HasFlag", e.reg(0), e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in pokemon(e).get("flags", ()) else 0

    def critical_stage(e):
        record("GetCriticalStage", e.reg(0))
        return pokemon(e).get("critical", 0)

    def friendship(e):
        record("GetFriendship", e.reg(0), e.reg(1))
        return STATE["pokemon"].get(e.reg(1), {}).get("friendship", 0)

    def rule(e):
        record("HasRule", e.reg(0), e.reg(1) & U32)
        return STATE["rule"]

    def unk1798(e):
        record("Unk1798::Check", e.reg(0), e.reg(1), e.reg(2) & U8)
        return 1 if (e.reg(2) & U8) in STATE["inPlay"] else 0

    def move_value(name):
        def get(e):
            record(name, e.reg(0) & U32)
            return STATE["moves"].get(e.reg(0) & U32, {}).get(name, 0)
        return get

    def nxt(e):
        n = e.reg(1)
        record("Next", e.reg(0), n)
        if n == 100 and STATE.get("roll") is not None:     # the hit check's roll, forced
            return STATE["roll"]
        return STATE["t"].rng.randrange(n) if n else 0
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(HAS_CONDITION, "HasCondition", has_condition)
    emu.stub_function(CONDITION_PARAM, "GetConditionParam", condition_param)
    emu.stub_function(HAS_FLAG, "HasFlag", has_flag)
    emu.stub_function(CRITICAL_STAGE, "GetCriticalStage", critical_stage)
    emu.stub_function(GET_FRIENDSHIP, "GetFriendship", friendship)
    emu.stub_function(HAS_RULE, "HasRule", rule)
    emu.stub_function(UNK1798_CHECK, "Unk1798::Check", unk1798)
    emu.stub_function(MOVE_ACCURACY, "GetAccuracy", move_value("accuracy"))
    emu.stub_function(NEVER_MISSES, "NeverMisses", move_value("neverMisses"))
    emu.stub_function(MOVE_CRITICAL_STAGE, "GetMoveCriticalStage", move_value("critical"))
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_hit_arithmetic(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    for accuracy in range(256):
        for stage in list(range(16)) + [0xFF, 0x106, r.getrandbits(32)]:
            t.check(t.call(ACCURACY_AFTER_STAGES, accuracy | r.choice([0, 0, 0x100, 0xFFFF00]), stage) & U8)
    for _ in range(600):
        stage = r.choice([0, 1, 2, 3, 3, 4, 6, 0xFF, 0x100, 0x102, r.getrandbits(32)])
        divisor = r.choice([1, 1, 2, 2, 3, 5, 9, 24, 25, 100, U32, U32 - 1, 0x80000000])
        t.check(t.call(ROLL_CRITICAL, stage, divisor) & U8)


class Battle:
    """A made-up battle: a flow object, Pokémon, moves, and handlers on the hit check's and the critical
    hit's events that change their variables in memory the way RewriteVar and MultiplyVar would. The handlers
    draw from a generator of their own, so a scene can be played again the same way (reseed)."""

    EVENTS = [0x3F, 0x40, 0x41, 0x42, 0x44, 0x87]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        self.flow = e.alloc(0x2900)
        for off, v in ((0x60, e.alloc(0x100)), (0x68, e.alloc(0x200)), (0x88, e.alloc(0x10))):
            e.write(self.flow + off, struct.pack("<Q", v))
        STATE["rule"] = r.choice([0, 0, 0, 0, 1])
        STATE["pokemon"] = {}
        STATE["inPlay"] = set(r.sample(range(4), r.randrange(4)))      # IDs whose No Guard counts
        STATE["moves"] = {}
        self.mons = [self.pokemon(i) for i in range(3)]
        t.call(RESET)
        STATE["battle"] = self
        if not hasattr(e, "handlers"):              # made once per test: the emulator has room for few
            e.handlers = {ev: e.host_function("on%x" % ev, Battle.handler(ev)) for ev in self.EVENTS}
        for ev in self.EVENTS:
            if r.random() < 0.6:
                table = e.alloc(struct.pack("<IIQ", ev, 0, e.handlers[ev]))
                t.call(ADD, r.choice([0, 1, 2, 3, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8),
                       r.choice([0, 1, 2]), table, 1)

    def pokemon(self, i):
        r = self.r
        p = self.e.alloc(0x400)
        stats = {"id": i}
        for value in (6, 7):
            stats[("value", value)] = r.choice([6, 6, 0, 0, 1, 5, 7, 11, 12, 12, r.randrange(13), 13, 0xFF, 0x106,
                                                r.getrandbits(32)])
        stats[("value", 0x11)] = r.choice([0, 0, 99, 99, 98, 100, 0x163, r.getrandbits(32)])
        stats[("value", 0xF)] = r.choice([1, 5, 50, 50, 51, 70, 100, 0x132, r.getrandbits(32)])
        stats["conditions"] = set(c for c in (0x11, 0x1D, 0x1E, 0x21) if r.random() < 0.15)
        stats["lockOn"] = r.choice([0, 1, 2, 3, 0x101, 0x1F])
        stats["flags"] = {0xE} if r.random() < 0.3 else set()
        stats["critical"] = r.choice([0, 0, 0, 1, 2, 3, 0xFE, 0x1FF])
        stats["friendship"] = r.choice([0, 0, 70, 199, 200, 201, 254, 255, 255, 0x1FF, 0x100])
        STATE["pokemon"][p] = stats
        return p

    def move(self):
        r = self.r
        move_id = r.choice([33, 85, 87, 524, r.getrandbits(16), r.getrandbits(32)])
        STATE["moves"][move_id] = {
            "accuracy": r.choice([0, 1, 10, 20, 30, 37, 50, 50, 60, 70, 75, 85, 90, 95, 100, 100, 255, 0x1234,
                                  r.getrandbits(32)]),
            "neverMisses": r.choice([0, 0, 0, 1, 0x100]),
            "critical": r.choice([0, 0, 0, 1, 2, 3, 6, 6, 7, 0xFF, 0x106]),
        }
        data = bytearray(r.getrandbits(8) for _ in range(0x20))
        struct.pack_into("<I", data, 0, move_id)
        struct.pack_into("<I", data, 0xC, r.choice([0, 1, 1, 2, 2, 3]))
        return self.e.alloc(bytes(data)), move_id

    def reseed(self, seed):
        self.hr.seed(seed)

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

    def read(self, label):
        i = self.slot(label)
        return None if i is None else struct.unpack("<i", self.e.read(VALUES + 4 * i, 4))[0]

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
            seen = tuple(self.full(l) for l in (2, 3, 4, 0x12, 0x1E, 0x2D, 0x2E, 0x31, 0x32, 0x3B, 0x4A, 0x54, 0x5B))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x3F:
                self.write(0x5B, r.choice([0, 1, 0x100]))
            elif event == 0x40:
                self.write(0x2D, r.choice([0, 3, 6, 9, 12, 13, -1, 0x106, r.getrandbits(32) - (1 << 31)]))
                self.write(0x2E, r.choice([0, 3, 6, 7, 9, 12, 13, -1, 0x107, r.getrandbits(32) - (1 << 31)]))
                self.write(0x54, r.choice([0, 0, 1, 0x100]))
                self.write(0x3B, r.choice([0x29, 0x800, 0x1000, 0x14CD, 0x2000, r.getrandbits(18)]))
            elif event in (0x41, 0x42):
                self.write(0x31, r.choice([0, 50, 100, 101, 255, 0x1FF, r.getrandbits(16)]))
                self.write(0x3B, r.choice([0x19A, 0x800, 0xE66, 0x1000, 0x14CD, 0x1A00, r.getrandbits(18)]))
            elif event == 0x44:
                self.write(0x32, r.choice([0, 1, 2, 3, 4, 6, -1, 0x103, r.getrandbits(32) - (1 << 31)]))
                self.write(0x4A, r.choice([0, 0, 0, 1, 0x100]))
            elif event == 0x87:
                self.write(0x4A, r.choice([0, 0, 1, 0x100]))
        return run


def test_always_hits(t):
    r = t.rng
    for _ in range(300):
        b = Battle(t)
        attacker, defender = r.choice(b.mons), r.choice(b.mons)
        t.check(t.call(ALWAYS_HITS, b.flow, attacker, defender) & U8)
        _, move_id = b.move()
        t.check(t.call(SKIPS_CHECK, b.flow, attacker, defender, move_id) & U8)
        t.check(t.emu.read(VARS, 0x5A4))


def test_check_hit(t):
    """Each scene is played again with the roll forced, to find the exact chance of a hit (bisection)."""
    r = t.rng
    for _ in range(300):
        b = Battle(t)
        attacker, defender = r.choice(b.mons), r.choice(b.mons)
        move, _ = b.move()
        dodged = t.emu.alloc(struct.pack("<B", 0xAA)) if r.random() < 0.8 else 0
        seed = r.getrandbits(32)
        low, high = 0, 101                         # the smallest roll that misses is in [low, high]
        while low < high:
            STATE["roll"] = (low + high) // 2
            b.reseed(seed)
            hit = t.call(CHECK_HIT, b.flow, attacker, defender, move, dodged) & U8
            t.check(hit)
            if dodged:
                t.check(t.emu.read(dodged, 1))
            low, high = (STATE["roll"] + 1, high) if hit else (low, STATE["roll"])
        STATE["roll"] = None
        t.check(("chance", low))
        b.reseed(seed)
        t.check(t.call(CALC_ACCURACY, b.flow, r.choice([0x41, 0x42, 0x3F]), attacker, defender, move) & U32)
        t.check(t.emu.read(VARS, 0x5A4))


def test_check_critical(t):
    r = t.rng
    for _ in range(400):
        b = Battle(t)
        attacker, defender = r.choice(b.mons), r.choice(b.mons)
        _, move_id = b.move()
        friendship = t.emu.alloc(struct.pack("<B", 0xAA))
        t.check(t.call(CHECK_CRITICAL, b.flow, attacker, defender, move_id, friendship) & U8)
        t.check(t.emu.read(friendship, 1))
        t.check(t.emu.read(VARS, 0x5A4))


def test_one_hit_ko(t):
    """Each scene is played again with the roll forced, to find the exact chance (bisection)."""
    r = t.rng
    for _ in range(300):
        b = Battle(t)
        attacker, defender = r.choice(b.mons), r.choice(b.mons)
        move, _ = b.move()
        seed = r.getrandbits(32)
        low, high = 0, 101
        while low < high:
            STATE["roll"] = (low + high) // 2
            b.reseed(seed)
            hit = t.call(ONE_HIT_KO, b.flow, attacker, defender, move) & U8
            t.check(hit)
            low, high = (STATE["roll"] + 1, high) if hit else (low, STATE["roll"])
        STATE["roll"] = None
        t.check(("chance", low))
        t.check(t.emu.read(VARS, 0x5A4))
