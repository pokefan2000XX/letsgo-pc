"""decomp/src/battle/damage.cpp and calc.cpp against the originals: the battle's arithmetic on edge and random
values, and a move's damage (power, attack, defense, the same-type bonus and the whole formula) on made-up
Pokémon, with made-up handlers changing the events' variables as moves, abilities and items do."""
import struct

BASE_DAMAGE, APPLY_EFFECTIVENESS, RANDOM, MUL_RATIO, WEATHER_RATIO = (
    0x71009CFAC0, 0x71009CFB00, 0x71009CFB90, 0x71009CFC10, 0x71009D01E0)
CALC_DAMAGE, CALC_POWER, CALC_ATTACK, CALC_DEFENSE, CALC_STAB = (
    0x7100A016D0, 0x7100A03C30, 0x7100A03D50, 0x7100A03F10, 0x7100A04170)
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
HAS_RULE, LIST_GET, GET_ID, GET_VALUE, GET_RAW, GET_STATUS, HAS_TYPE, GET_CRIT, NEXT, GET_WEATHER = (
    0x71009DA1F0, 0x71009E0DE0, 0x71009E2F30, 0x71009E3690, 0x71009E3CB0, 0x71009E3F70, 0x71009E4EF0, 0x71009E5150,
    0x71009E89F0, 0x7100A681C0)
GET_POKEMON = 0x7100A095E0

VARS = 0x710164D770
LABELS, VALUES = VARS + 4, VARS + 0xC4
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

    def value(kind):
        def get(e):
            stat = e.reg(1) & U32
            record(kind, e.reg(0), stat)
            return pokemon(e).get((kind, stat), 0)
        return get

    def has_type(e):
        record("HasType", e.reg(0), e.reg(1) & U8)
        return 1 if (e.reg(1) & U8) in pokemon(e).get("types", ()) else 0

    def status(e):
        record("GetStatus", e.reg(0))
        return pokemon(e).get("status", 0)

    def list_get(e):
        record("ListGet", e.reg(0), e.reg(1) & U8)
        return STATE["by_id"].get(e.reg(1) & U8, 0)

    def weather(e):
        record("GetWeather", e.reg(0))
        return STATE["weather"]

    def rule(e):
        record("HasRule", e.reg(0), e.reg(1) & U32)
        return STATE["rule"]

    def nxt(e):
        n = e.reg(1)
        record("Next", n)
        return STATE["t"].rng.randrange(n) if n else 0
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", value("GetValue"))
    emu.stub_function(GET_RAW, "GetValueIgnoringStages", value("GetValueIgnoringStages"))
    emu.stub_function(GET_CRIT, "GetValueForCritical", value("GetValueForCritical"))
    emu.stub_function(HAS_TYPE, "HasType", has_type)
    emu.stub_function(GET_STATUS, "GetStatus", status)
    emu.stub_function(LIST_GET, "ListGet", list_get)
    emu.stub_function(GET_WEATHER, "GetWeather", weather)
    emu.stub_function(HAS_RULE, "HasRule", rule)
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_arithmetic(t):
    r = t.rng
    edges = [0, 1, 2, 5, 0x7F, 0xFF, 0x100, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, U32]
    for _ in range(3000):
        a, b, c, d = (r.choice(edges + [r.getrandbits(r.choice([8, 16, 32]))]) for _ in range(4))
        t.check(t.call(BASE_DAMAGE, a, b, c, d) & U32)
        t.check(t.call(MUL_RATIO, a, b) & U32)
        t.check(t.call(APPLY_EFFECTIVENESS, a, r.choice([r.randrange(16), b])) & U32)
        t.check(t.call(WEATHER_RATIO, r.choice([r.randrange(9), c]), r.choice([9, 10, 5, 0x12, r.randrange(20), d])) & U32)
    STATE["t"] = t
    for n in (1, 2, 16, 100, 0x10000, U32):
        t.check(t.call(RANDOM, n) & U32)


class Battle:
    """A made-up battle: a flow object, Pokémon, a field, and handlers on the damage's events that change
    their variables in memory the way RewriteVar and MultiplyVar would."""

    EVENTS = [0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x53, 0x54, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x92]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        self.flow = e.alloc(0x2500)
        switch = e.alloc(0x30)
        e.write(switch + 0x20, struct.pack("<I", r.choice([0, 0, 1])))
        for off, v in ((0x60, e.alloc(0x100)), (0x68, e.alloc(0x200)), (0x88, e.alloc(0x10)), (0x24E0, switch)):
            e.write(self.flow + off, struct.pack("<Q", v))
        STATE["weather"] = r.choice([0, 1, 2, 3, 4, 4, 4, 5, 6, 7])      # sandstorm often: Rock types' special defense
        STATE["rule"] = r.choice([0, 0, 0, 1])
        STATE["pokemon"], STATE["by_id"] = {}, {}
        self.mons = [self.pokemon(i) for i in range(3)]
        t.call(RESET)
        self.funcs = {ev: e.host_function("on%x" % ev, self.handler(ev)) for ev in self.EVENTS}
        for ev in self.EVENTS:
            if r.random() < 0.6:
                table = e.alloc(struct.pack("<IIQ", ev, 0, self.funcs[ev]))
                t.call(ADD, r.choice([0, 1, 2, 3, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8),
                       r.choice([0, 1, 2]), table, 1)

    def pokemon(self, i):
        r = self.r
        p = self.e.alloc(0x400)
        stats = {}
        for kind in ("GetValue", "GetValueIgnoringStages", "GetValueForCritical"):
            for stat in (8, 9, 10, 11):
                stats[(kind, stat)] = r.choice([0, 1, 50, r.randrange(1000), r.getrandbits(16), r.getrandbits(32)])
        stats[("GetValue", 0xF)] = r.choice([1, 5, 50, 100, r.getrandbits(8), r.getrandbits(16)])
        stats["id"] = i
        stats["types"] = set(r.sample([9, 10, 0, 1, 12, 0x12], r.randrange(3))) | ({5} if r.random() < 0.4 else set())
        stats["status"] = r.choice([0, 0, 4, 4, 1, 2])
        STATE["pokemon"][p] = stats
        STATE["by_id"][i] = p
        return p

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

    def handler(self, event):
        def run(e):
            r = self.r
            seen = tuple(self.read(l) for l in (2, 3, 4, 0x12, 0x19, 0x1E, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3D, 0x3E,
                                                0x3F, 0x42, 0x43, 0x44, 0x4A, 0x4D, 0x4E, 0x4F, 0x5B, 0x5C, 0x65, 0x1C))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x59 and r.random() < 0.5:
                self.write(0x3D, r.choice([0, 1, 20, r.getrandbits(16), r.getrandbits(32) - (1 << 31)]))
                self.write(0x3E, r.choice([0, 1, 2]))
            elif event == 0x46:
                self.write(0x36, r.choice([0, 10, 120, r.getrandbits(16), r.getrandbits(20)]))
            elif event in (0x47, 0x4A, 0x4B, 0x5B):
                label = {0x47: 0x37, 0x4A: 0x3B, 0x4B: 0x3B, 0x5B: 0x3B}[event]
                self.write(label, r.choice([0, 0x800, 0x1800, 0x2000, 0x5000, r.getrandbits(22)]))
                if event == 0x4A:
                    self.write(0x5C, r.choice([0, 1]))
                    self.write(0x39, r.choice([1, 100, r.getrandbits(16)]))
                if event == 0x4B:
                    self.write(0x3A, r.choice([0, 1, 100, r.getrandbits(16)]))
                if event == 0x5B:
                    self.write(0x38, r.choice([0, 1, 100, r.getrandbits(32) - (1 << 31)]))
            elif event == 0x48:
                self.write(0x42, r.choice([0, 1, 2, 0x1F, 0x11F, 0x101]))
                self.write(0x5B, r.choice([0, 1]))
            elif event == 0x49:
                self.write(0x44, r.choice([0, 1, 2, 3]))
                self.write(0x5B, r.choice([0, 1, 0x100]))
            elif event == 0x53:
                self.write(0x5B, r.choice([0, 1, 2]))
            elif event == 0x54:
                self.write(0x3B, r.choice([0x1000, 0x2000, 0x1800, r.getrandbits(16)]))
            elif event == 0x5C:
                self.write(0x3C, r.choice([0, 0x800, 0x1000, 0x3000, r.getrandbits(20)]))
            elif event == 0x92:
                self.write(0x4A, r.choice([0, 1]))
        return run

    def move(self):
        r = self.r
        data = bytearray(r.getrandbits(8) for _ in range(0x20))
        struct.pack_into("<I", data, 0, r.choice([33, 85, 263, r.getrandbits(16)]))
        data[0xA] = r.choice([0, 5, 9, 10, 12, 0x12, r.randrange(19)])
        struct.pack_into("<I", data, 0xC, r.choice([0, 1, 1, 2, 2, 3]))
        struct.pack_into("<H", data, 0x1C, r.choice([0, 1, 40, 90, 120, 250, r.getrandbits(16)]))
        return self.e.alloc(bytes(data))

    def vars(self):
        return self.e.read(VARS, 0x5A4)


def test_damage(t):
    r = t.rng
    for scene in range(150):
        b = Battle(t)
        attacker, defender = r.sample(b.mons, 2)
        move = b.move()
        out = t.emu.alloc(8)
        t.emu.write(out, b"\xcc" * 8)
        fixed = t.call(CALC_DAMAGE, b.flow, attacker, defender, move, r.choice([0, 1, 3, 6, 7, 7, 8, 9, 13, 14]),
                       r.choice([0x1000, 0x1000, 0xC00, 0x800]), r.choice([0, 1]), r.choice([0, 1]),
                       r.choice([0, 1]), out)
        t.check((fixed & U8 != 0, t.emu.read(out, 8)))
        t.check(b.vars())


def test_damage_parts(t):
    r = t.rng
    for scene in range(150):
        b = Battle(t)
        attacker, defender = r.sample(b.mons, 2)
        move = b.move()
        part = r.randrange(4)
        if part == 0:
            t.check(t.call(CALC_POWER, b.flow, attacker, defender, move) & U16)
        elif part == 1:
            out = t.emu.alloc(8)
            t.emu.write(out, b"\xcc" * 8)
            t.check((t.call(CALC_ATTACK, b.flow, attacker, defender, move, r.choice([0, 1]), out) & U16, t.emu.read(out, 8)))
        elif part == 2:
            t.check(t.call(CALC_DEFENSE, b.flow, attacker, defender, move, r.choice([0, 1])) & U16)
        else:
            t.check(t.call(CALC_STAB, b.flow, attacker, r.choice([0, 5, 9, 10, 0x12, 0x109])) & U32)
        t.check(b.vars())
