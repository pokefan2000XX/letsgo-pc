"""decomp/src/battle/add_condition.cpp and calc.cpp's sun check against the originals: whether conditions can
be given to made-up Pokémon (their conditions, statuses, types and abilities made up, the weather too), with
made-up handlers on the events that let abilities, items and moves stop a condition or let it past a type,
and the failure messages for every reason and condition."""
import random
import struct

IS_SUNNY = 0x71009D0250
PREVENTED, IGNORES_IMMUNITY, IMMUNITY, IMMUNE_EVENT, FAIL_MESSAGE = (
    0x71009FDE60, 0x7100A04EF0, 0x7100A04FB0, 0x7100A05360, 0x7100A051F0)
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, GET_VALUE, HAS_CONDITION, GET_STATUS, GET_TYPES, PERMANENT, GET_WEATHER = (
    0x71009E2F30, 0x71009E3690, 0x71009E3DA0, 0x71009E3F70, 0x71009E4E50, 0x7100A18FE0, 0x7100A681C0)
SAVE, RESTORE, PUT_MESSAGE, GET_POKEMON = 0x7100A68E80, 0x7100A68EE0, 0x7100A12B40, 0x7100A095E0

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def record(name, *args):
        STATE["t"].check((name,) + args)

    def simple(name, key):
        def get(e):
            record(name, e.reg(0))
            return pokemon(e).get(key, 0)
        return get

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get(("value", e.reg(1) & U32), 0)

    def has_condition(e):
        record("HasCondition", e.reg(0), e.reg(1) & U32)
        return 1 if (e.reg(1) & U32) in pokemon(e).get("conditions", ()) else 0

    def permanent(e):
        record("IsAbilityPermanent", e.reg(0) & U32)
        return 1 if (e.reg(0) & U32) in (121, 176, 0x179) else 0

    def weather(e):
        record("GetWeather", e.reg(0))
        return STATE["weather"]

    def save(e):
        record("Save", e.reg(0))
        return STATE["saved"]

    def restore(e):
        record("Restore", e.reg(0), e.reg(1) & U32)

    def put_message(e):                              # up to the 0xffff0000 that ends them
        args = []
        for i in range(1, 8):
            args.append(e.reg(i) & U32)
            if args[-1] == 0xFFFF0000:
                break
        record("PutMessage", e.reg(0), *args)
    emu.stub_function(GET_ID, "GetId", simple("GetId", "id"))
    emu.stub_function(GET_STATUS, "GetStatus", simple("GetStatus", "status"))
    emu.stub_function(GET_TYPES, "GetTypes", simple("GetTypes", "types"))
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(HAS_CONDITION, "HasCondition", has_condition)
    emu.stub_function(PERMANENT, "IsAbilityPermanent", permanent)
    emu.stub_function(GET_WEATHER, "GetWeather", weather)
    emu.stub_function(SAVE, "Save", save)
    emu.stub_function(RESTORE, "Restore", restore)
    emu.stub_function(PUT_MESSAGE, "PutMessage", put_message)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_is_sunny(t):
    for weather in list(range(0x10)) + [0x101, 0x106, 0xFF01, U32]:
        t.check(t.call(IS_SUNNY, weather) & U8)


TYPES = [0, 3, 8, 9, 0xB, 0xC, 0xE, 0x12, 0x1F]       # Normal, Poison, Steel, Fire, Grass, Electric, Ice


class Battle:
    """A made-up battle: a flow object, two Pokémon, and handlers on the events that change their variables in
    memory the way RewriteVar would. The handlers draw from a generator of their own."""

    EVENTS = [0x7C, 0x7E, 0x92, 0xD3]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400)]
        self.flow, setup_obj, mons, commands, field, self.target, self.attacker = e.scene
        for addr, n in zip(e.scene, (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400)):
            e.write(addr, bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(self.flow + 0x1511, bytes([r.choice([0, 0x01, 0x20, 0x80, 0xFF])]))
        STATE["weather"] = r.choice([0, 0, 1, 1, 2, 4, 5, 6, 6, 0x101])
        STATE["saved"] = r.getrandbits(32)
        for i, p in enumerate((self.target, self.attacker)):
            STATE["pokemon"][p] = {
                "id": r.choice([i, i, 5, 0x101]),
                "status": r.choice([0, 0, 0, 1, 2, 3, 4, 5]),
                "conditions": set(c for c in range(0x16) if r.random() < 0.08),
                "types": (r.choice(TYPES) | r.choice(TYPES) << 5 | r.choice(TYPES) << 10
                          | r.choice([0, 0, 0x8000, 0x10000])),
                ("value", 0x10): r.choice([0, 121, 176, 0x179, 5, r.getrandbits(32)]),
            }
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
            seen = tuple(self.full(l) for l in (3, 4, 0x1D, 0x21, 0x4A, 0x4C, 0x5B, 0x6C))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x7C:
                self.write(0x4A, r.choice([0, 1, 1, 0x100]))
                self.write(0x4C, r.choice([0, 1, 0x100]))
            elif event == 0x92:
                self.write(0x4A, r.choice([0, 1, 0x100]))
            elif event == 0xD3:
                self.write(0x5B, r.choice([0, 1, 0x100]))
        return run


CONDITIONS = [1, 2, 3, 4, 5, 6, 3, 4, 5, 1, 0xE, 0x10, 0x12, 0xF, 0x14, 7, 0x11, 0x13, 0x15, 0, -1, 0x7FFFFFFF]


def test_condition_prevented(t):
    r = t.rng
    for _ in range(2500):
        b = Battle(t)
        condition = r.choice(CONDITIONS) if r.random() < 0.9 else r.randrange(0x16)
        attacker = b.attacker if r.random() < 0.85 else 0
        t.check(t.call(PREVENTED, b.flow, b.target, attacker, condition & U32, r.getrandbits(64),
                       r.choice([0, 1, 3, 3, 4]), r.getrandbits(32), r.choice([0, 0, 1, 2, 3]),
                       r.choice([0, 1, 1, 0x100, 0x101]), r.choice([0, 1, 0x100])) & U8)
        t.check(t.emu.read(b.flow + 0x1511, 1))
        t.check(t.emu.read(VARS, 0x5A4))


def test_condition_parts(t):
    r = t.rng
    for _ in range(600):
        b = Battle(t)
        condition = r.choice(CONDITIONS) & U32
        attacker = b.attacker if r.random() < 0.8 else 0
        cause = r.choice([0, 1, 3, 3, 4])
        t.check(t.call(IGNORES_IMMUNITY, b.flow, b.target, attacker, condition, cause) & U8)
        t.check(t.call(IMMUNITY, b.flow, b.target, condition) & U32)
        message = t.emu.alloc(bytes([r.choice([0, 1])]))
        t.check(t.call(IMMUNE_EVENT, b.flow, attacker, b.target, condition, cause, message) & U8)
        t.check(t.emu.read(message, 1))
        for reason in (r.choice([0, 1, 1, 2, 2, 3, 4]),):
            t.call(FAIL_MESSAGE, b.flow, b.target, reason, condition, cause)
        t.check(t.emu.read(b.flow + 0x1511, 1))
        t.check(t.emu.read(VARS, 0x5A4))
