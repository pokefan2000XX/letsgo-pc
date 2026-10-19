"""decomp/src/battle/add_condition.cpp's move conditions against the originals: whether a made-up move's
condition can be given, and giving one (whether the check stops it made up too), with made-up handlers on the
events that pick the condition, see it and change its 8 bytes, and that may leave a message."""
import random
import struct

from unicorn.arm64_const import UC_ARM64_REG_SP

CAN_GIVE, GIVE = 0x71009FDD30, 0x7100A04CF0
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, NEXT, GET_POKEMON = 0x71009E2F30, 0x71009E89F0, 0x7100A095E0
INFLICT_CONDITION, INFLICT_DURATION = 0x710073CE20, 0x710073CF00
PREVENTED, ADD_CONDITION, PUT_BATTLE_MESSAGE = 0x71009FDE60, 0x71009FF450, 0x71009FA760

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STACK_HIGH, STACK_END = 0x73, 0x110000           # a pointer to the stack in two variables: 0x73, then the rest
MESSAGE = 0x2720                                 # Flow::message
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def get_id(e):
        record("GetId", e.reg(0))
        return STATE["pokemon"].get(e.reg(0), {}).get("id", 0x1F)

    def move_value(name):
        def get(e):
            record(name, e.reg(0) & U32)
            return STATE["move"][name]
        return get

    def nxt(e):
        n = e.reg(1)
        record("Next", e.reg(0), n)
        return STATE["t"].rng.randrange(n) if n else 0

    def stack(e, n):
        sp = e.uc.reg_read(UC_ARM64_REG_SP)
        return tuple(e.read(sp + 8 * i, 1)[0] for i in range(n))

    def prevented(e):                                # its conditionData isn't read (nor always passed)
        record("CheckConditionPrevented", e.reg(0), e.reg(1), e.reg(2), e.reg(3) & U32, e.reg(5) & U32,
               e.reg(6) & U32, e.reg(7) & U32, *stack(e, 2))
        return STATE["prevented"]

    def add_condition(e):
        record("AddCondition", e.reg(0), e.reg(1), e.reg(2), e.reg(3) & U32, e.read(e.reg(4), 8), e.reg(5) & U8,
               e.reg(6) & U32, e.reg(7))

    def put_battle_message(e):
        record("PutBattleMessage", e.reg(0), e.reg(1), e.read(e.reg(1), 0x24))
        return 1
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(INFLICT_CONDITION, "GetInflictCondition", move_value("GetInflictCondition"))
    emu.stub_function(INFLICT_DURATION, "GetInflictDuration", move_value("GetInflictDuration"))
    emu.stub_function(NEXT, "Next", nxt)
    emu.stub_function(PREVENTED, "CheckConditionPrevented", prevented)
    emu.stub_function(ADD_CONDITION, "AddCondition", add_condition)
    emu.stub_function(PUT_BATTLE_MESSAGE, "PutBattleMessage", put_battle_message)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


class Battle:
    """A made-up battle: a flow object with its message, two Pokémon, a move, and handlers on the events that
    change their variables in memory the way RewriteVar would, the condition's 8 bytes and the message. The
    handlers draw from a generator of their own."""

    EVENTS = [0x77, 0x78, 0x79]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        sizes = (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400)
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in sizes]
        self.flow, setup_obj, mons, commands, field, self.attacker, self.defender = e.scene
        for addr, n in zip(e.scene, sizes):
            e.write(addr, bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(self.flow + MESSAGE, bytes(r.getrandbits(8) for _ in range(0x24)))
        self.move_id = r.choice([33, 85, 0x1234, r.getrandbits(32)])
        STATE["move"] = {
            "GetInflictCondition": r.choice([0, 1, 2, 4, 5, 7, 0x12, 0xFFFF, 0xFFFF, 0x10000]),
            # kinds 1 to 4 only: for others the original passes whatever is on its stack (not read)
            "GetInflictDuration": r.choice([1, 2, 3, 4]) | r.randrange(64) << 4 | r.randrange(64) << 10
            | r.choice([0, 0x10000]),
        }
        STATE["prevented"] = r.choice([0, 0, 1, 0x100])
        for i, p in enumerate((self.attacker, self.defender)):
            STATE["pokemon"][p] = {"id": r.choice([i, i, 5, 0x101])}
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

    def pointer(self, label):
        high, low = self.full(label), self.full(label + 1)
        return None if high is None or low is None else (high[0] & U32) << 32 | (low[0] & U32)

    @staticmethod
    def handler(event):
        def run(e):
            self = STATE["battle"]
            r = self.hr
            seen = tuple(self.full(l) for l in (3, 4, 0x21))
            data, message = self.pointer(0x23), self.pointer(0x47)
            self.t.check(("event", event, e.reg(2) & U8, None if data is None else self.e.read(data, 8),
                          None if message is None else message - self.flow) + seen)
            if r.random() < 0.3:
                return
            if event == 0x77:
                self.write(0x21, r.choice([0, 1, 4, 7, 0xFFFF, 0x10000]))
            if data is not None and r.random() < 0.5:
                self.e.write(data, struct.pack("<Q", r.getrandbits(64)))
            if message is not None and r.random() < 0.4:      # a message, or none (byte 2's low 7 bits)
                self.e.write(message + 2, bytes([r.choice([0, 0x80, 1, 0x7F])]))
        return run


def variables(t):
    """The variables after a call, without a pointer to the stack (which differs between the two runs)."""
    data = bytearray(t.emu.read(VARS, 0x5A4))
    values = list(struct.unpack_from("<96I", data, 0xC4))
    for i in range(95):
        if values[i] == STACK_HIGH and values[i + 1] < STACK_END:
            struct.pack_into("<II", data, 0xC4 + 4 * i, 0, 0)
    return bytes(data)


def test_give_move_condition(t):
    r = t.rng
    for _ in range(2000):
        b = Battle(t)
        attacker, defender = (b.attacker, b.defender) if r.random() < 0.8 else (b.defender, b.attacker)
        if r.random() < 0.4:
            t.check(t.call(CAN_GIVE, b.flow, b.move_id, attacker, defender) & U8)
        else:
            condition = r.choice([1, 2, 4, 5, 7, 8, 0x12, 0xFFFF, 0xFFFF, 0x10000])
            t.check(t.call(GIVE, b.flow, attacker, defender, r.getrandbits(32), condition, r.getrandbits(64),
                           r.choice([0, 1, 3, r.getrandbits(32)]), r.choice([0, 1, r.getrandbits(32)]),
                           r.choice([0, 1]), r.choice([0, 1])) & U8)
        t.check(t.emu.read(b.flow + MESSAGE, 0x24))
        t.check(variables(t))
