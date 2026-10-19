"""decomp/src/battle/order.cpp and order_key.cpp against the originals: turn order keys on every kind of
action, speeds, and whole turns put in order on made-up Pokémon, with made-up handlers changing speed,
priority and brackets as abilities and items do, and equal keys left to the (faked) random numbers."""
import struct

ORDER_ACTIONS, CALC_ORDER_KEY, CALC_SPEED = 0x71009F3080, 0x71009F4030, 0x71009F4190
MAKE_KEY, SET_BRACKET, WITHOUT_KIND, KEY_PRIORITY = 0x7100A50E80, 0x7100A50F30, 0x7100A50F40, 0x7100A50F50
RESET, ADD = 0x7100A66740, 0x7100A667F0
GET_ID, GET_VALUE, GET_STATUS, NEXT = 0x71009E2F30, 0x71009E3690, 0x71009E3F70, 0x71009E89F0
GET_MOVE, GET_PRIORITY, GET_TYPE, PUT = 0x7100A51270, 0x710073CAA0, 0x710073C950, 0x7100A11D70
GET_POKEMON = 0x7100A095E0

VARS = 0x710164D770
LABELS, VALUES = VARS + 4, VARS + 0xC4
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STATE = {}


def setup(emu):
    def record(*args):
        STATE["t"].check(args)

    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def get_id(e):
        record("GetId", e.reg(0))
        return pokemon(e).get("id", 0x1F)

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("speed", 0) if (e.reg(1) & U32) == 0xC else 0

    def status(e):
        record("GetStatus", e.reg(0))
        return pokemon(e).get("status", 0)

    def get_move(e):
        info = struct.unpack("<I", e.read(e.reg(0) + 8, 4))[0]
        record("GetMove", e.reg(0))
        return info >> 4 & 0xFFF if info & 0xF == 1 else 0

    def priority(e):
        record("GetPriority", e.reg(0) & U32)
        return [0, 1, 2, -1, -3, 4, 5, -7][(e.reg(0) & U32) % 8] & U32

    def move_type(e):
        record("GetType", e.reg(0) & U32)
        return (e.reg(0) & U32) % 19

    def put(e):
        record("Put", e.reg(0), e.reg(1) & U32, e.reg(2) & U8, e.reg(3) & U32)

    def nxt(e):
        n = e.reg(1)
        record("Next", n)
        return STATE["t"].rng.randrange(n) if n else 0
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(GET_STATUS, "GetStatus", status)
    emu.stub_function(GET_MOVE, "GetMove", get_move)
    emu.stub_function(GET_PRIORITY, "GetPriority", priority)
    emu.stub_function(GET_TYPE, "GetType", move_type)
    emu.stub_function(PUT, "Put", put)
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)


def test_order_keys(t):
    r = t.rng
    info = t.emu.alloc(8)
    for _ in range(4000):
        t.emu.write(info, struct.pack("<I", r.choice([r.randrange(16), r.getrandbits(32)])))
        args = (r.choice([0, 0, 1, r.getrandbits(32)]), 0x7101230000, info, r.choice([0, 1, 1, 2, 0x101]),
                r.choice([0, 1, 100, 0x1FFF, 0x2000, 10000, r.getrandbits(32)]), r.getrandbits(16),
                r.choice([0, 6, 7, 8, 14, 0x3F, 0x40, r.getrandbits(32)]))
        t.check(t.call(MAKE_KEY, *args) & U32)
        key = r.getrandbits(32)
        t.check((t.call(SET_BRACKET, key, r.choice([0, 1, 2, 7, 8, r.getrandbits(32)])) & U32,
                 t.call(WITHOUT_KIND, key) & U32, t.call(KEY_PRIORITY, key) & U32))


class Turn:
    """A made-up turn: a flow object, Pokémon, actions, and handlers on the turn order's events."""

    def __init__(self, t, ties=False):
        self.t, self.e, self.r = t, t.emu, t.rng
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        self.flow = e.alloc(0x2900)
        mode = e.alloc(struct.pack("<I", r.choice([0, 0, 1, 2])))
        setup_obj = e.alloc(0x100)
        e.write(setup_obj + 0x80, struct.pack("<Q", mode))
        e.write(self.flow + 0x60, struct.pack("<Q", setup_obj))
        e.write(self.flow + 0x70, struct.pack("<Q", e.alloc(0x40)))
        e.write(self.flow + 0x88, struct.pack("<Q", e.alloc(0x100)))     # the field, for items' skip checks
        e.write(self.flow + 0x2808, struct.pack("<I", r.getrandbits(32)))
        self.mons = []
        tied = r.choice([50, 100])
        for i in range(r.randrange(2, 4) if ties else r.randrange(2, 7)):
            p = e.alloc(0x400)
            e.write(p + 0x2E6, bytes([r.choice([0, 1, 1, 2, 7])]))
            speed = tied if ties else r.choice([0, 1, 50, 100, 100, 9999, 10000, 10001, 10015, 10016, 20000,
                                                r.getrandbits(16), r.getrandbits(32)])
            STATE["pokemon"][p] = {"id": i, "speed": speed, "status": 0 if ties else r.choice([0, 0, 1, 1, 4])}
            self.mons.append(p)
        t.call(RESET)
        for ev in (0xF, 0x11, 0x13):
            if not ties and r.random() < 0.7:
                fn = e.host_function("on%x" % ev, self.handler(ev))
                table = e.alloc(struct.pack("<IIQ", ev, 0, fn))
                t.call(ADD, r.choice([0, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8), r.choice([0, 1]), table, 1)

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
            self.t.check(("event", event, e.reg(2) & U8) + tuple(self.read(l) for l in (2, 3, 0x11, 0x12, 0x19, 0x1C, 0x35,
                                                                                         0x3B, 0x53, 0x5B)))
            if r.random() < 0.3:
                return
            if event == 0xF:
                self.write(0x11, r.choice([0, 1, 2, 3, 9]))
            elif event == 0x11:
                self.write(0x1C, r.choice([0, 7, 8, 10, 14, 0x40, -1]))
            else:
                self.write(0x35, r.choice([0, 1, 100, 300, 10001, 10015, r.getrandbits(16)]))
                self.write(0x3B, r.choice([0x800, 0x1000, 0x2000, r.getrandbits(18)]))
                self.write(0x5B, r.choice([0, 1]))
                self.write(0x53, r.choice([0, 0, 1]))
        return run

    def actions(self, n, ties=False):
        r = self.r
        data = b""
        for _ in range(n):
            kind = r.choice([1, 2, 3]) if ties else r.choice([1, 1, 1, 1, 1, 2, 3, 4, 5, 7, 9, 0, 11])
            move = r.choice([8, 16, 24]) if ties else r.randrange(1 << 12)      # ties: moves of priority 0
            info = kind | move << 4 | r.getrandbits(8) << 24
            data += struct.pack("<QI4sIBBBB", r.choice(self.mons), info, bytes(4), r.getrandbits(32),
                                r.choice([0, 1, 2]), 0, 0, r.choice([0, 1]))
        return self.e.alloc(data), data


def test_speed(t):
    r = t.rng
    for _ in range(200):
        turn = Turn(t)
        t.check(t.call(CALC_SPEED, turn.flow, r.choice(turn.mons), r.choice([0, 1])) & U32)


def test_order_actions(t):
    r = t.rng
    for scene in range(160):
        ties = scene % 2 == 1                      # every other turn: equal keys, left to the coin
        turn = Turn(t, ties)
        n = r.randrange(1, 9)
        actions, _ = turn.actions(n, ties)
        if r.random() < 0.2:
            t.check(t.call(CALC_ORDER_KEY, turn.flow, actions + 0x18 * r.randrange(n)) & U32)
        first, last = r.randrange(n), r.randrange(n)
        if r.random() < 0.7:
            first, last = min(first, last), max(first, last)
        t.call(ORDER_ACTIONS, turn.flow, actions, first, last, r.choice([0, 1]))
        t.check(t.emu.read(actions, 0x18 * n))
        t.check([t.emu.read(p + 0x2E6, 1) for p in turn.mons])
        t.check(t.emu.read(turn.flow + 0x2808, 4))
        t.check(t.emu.read(VARS, 0x5A4))
