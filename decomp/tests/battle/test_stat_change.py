"""decomp/src/battle/stat_change.cpp against the originals: whether made-up Pokémon's stat stages can change
(the stage limit, a substitute, the battle being decided all made up), with made-up handlers on the events
that abilities answer (Simple, Clear Body, Defiant), and the change itself with its messages and failures."""
import random
import struct

CAN_CHANGE, CHANGE, ALLOWED = 0x7100A05830, 0x7100A05A80, 0x7100A05C10
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, IS_SIDE_OUT, CAN_CHANGE_STAGE, HAS_SUBSTITUTE = 0x71009E2F30, 0x71009F2D80, 0x71009E54F0, 0x71009E7940
SAVE, RESTORE, PUT_MESSAGE, GET_POKEMON = 0x7100A68E80, 0x7100A68EE0, 0x7100A12B40, 0x7100A095E0
PUT_BATTLE_MESSAGE, APPLY_STAT_STAGE = 0x71009FA760, 0x71009FA4B0

VARS = 0x710164D770
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
FLOW_SIZE, FAILURES = 0x2D00, 0x2C1C
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def record(name, *args):
        STATE["t"].check((name,) + args)

    def get_id(e):
        record("GetId", e.reg(0))
        return pokemon(e).get("id", 0x1F)

    def side_out(e):
        record("IsSideOut", e.reg(0))
        return STATE["sideOut"]

    def can_change_stage(e):
        record("CanChangeStage", e.reg(0), e.reg(1) & U32, e.reg(2) & U32)
        return pokemon(e).get("canChange", 0)

    def substitute(e):
        record("HasSubstitute", e.reg(0))
        return pokemon(e).get("substitute", 0)

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

    def put_battle_message(e):
        record("PutBattleMessage", e.reg(0), e.reg(1))
        return 1

    def apply_stat_stage(e):
        record("ApplyStatStage", e.reg(0), e.reg(1), e.reg(2) & U32, e.reg(3) & U32, e.reg(4) & U32,
               e.reg(5) & U8, e.reg(6) & U8)
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(IS_SIDE_OUT, "IsSideOut", side_out)
    emu.stub_function(CAN_CHANGE_STAGE, "CanChangeStage", can_change_stage)
    emu.stub_function(HAS_SUBSTITUTE, "HasSubstitute", substitute)
    emu.stub_function(SAVE, "Save", save)
    emu.stub_function(RESTORE, "Restore", restore)
    emu.stub_function(PUT_MESSAGE, "PutMessage", put_message)
    emu.stub_function(PUT_BATTLE_MESSAGE, "PutBattleMessage", put_battle_message)
    emu.stub_function(APPLY_STAT_STAGE, "ApplyStatStage", apply_stat_stage)
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


class Battle:
    """A made-up battle: a flow object (with its list of failures), two Pokémon, and handlers on the stat
    change's events that change their variables in memory the way RewriteVar would. The handlers draw from a
    generator of their own."""

    EVENTS = [0x71, 0x72, 0x73, 0x74]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.hr = random.Random(t.rng.getrandbits(32))
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        sizes = (FLOW_SIZE, 0x200, 0x200, 0x40, 0x300, 0x400, 0x400, 0x40)
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in sizes]
        self.flow, setup_obj, mons, commands, field, self.target, self.other, self.message = e.scene
        for addr, n in zip(e.scene, sizes):
            e.write(addr, bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(setup_obj + 0x168, struct.pack("<I", r.choice([0, 1, 2, r.getrandbits(32)])))
        e.write(self.flow + 0x1511, bytes([r.choice([0, 0x20, 0x80, 0x5F, 0xFF])]))
        failures = struct.pack("<5III", *(r.getrandbits(32) for _ in range(5)), r.choice([0, 1, 3, 4, 5, 6, 0x100]),
                               r.choice([0, 1, 1, 1, 0x100]))
        e.write(self.flow + FAILURES, failures)
        e.write(self.message, bytes(r.getrandbits(8) for _ in range(0x24)))
        STATE["sideOut"] = 1 if r.random() < 0.1 else 0
        STATE["saved"] = r.getrandbits(32)
        for p in (self.target, self.other):
            STATE["pokemon"][p] = {"id": r.choice([0, 1, 2, 3, 5]), "canChange": 1 if r.random() < 0.75 else 0,
                                   "substitute": 1 if r.random() < 0.3 else 0}
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
            seen = tuple(self.full(l) for l in (2, 3, 0x1D, 0x25, 0x26, 0x4A, 0x69))
            self.t.check(("event", event, e.reg(2) & U8) + seen)
            if r.random() < 0.3:
                return
            if event == 0x71:
                self.write(0x26, r.choice([-12, -6, -2, -1, 0, 1, 2, 3, 6, 12, 0x100]))
            elif event == 0x72:
                self.write(0x4A, r.choice([0, 0, 1, 0x100]))
        return run


def test_stat_change(t):
    r = t.rng
    for _ in range(2000):
        b = Battle(t)
        target, other = (b.target, b.other) if r.random() < 0.5 else (b.other, b.target)
        ids = [STATE["pokemon"][p]["id"] for p in (target, other)]
        causer = r.choice([ids[0], ids[0], ids[1], 0x1F]) | r.choice([0, 0, 0x100, 0xFFFFFF00])
        stat = r.choice([1, 2, 3, 4, 5, 6, 7, 0, 8, 0x101])
        stages = r.choice([-3, -2, -1, 1, 2, 3, 0, 6, -6, 12]) & U32
        unk69 = r.choice([0, 1, 2, r.getrandbits(32)])
        unk3 = r.choice([ids[0], ids[1], 0x1F]) | r.choice([0, 0, 0x300, 0xFFFFFF00])
        unk7 = r.getrandbits(32)
        unk1d = r.choice([0, 1, 2, 0x10, r.getrandbits(32)])
        flags = [r.choice([0, 1]) for _ in range(5)]
        which = r.randrange(3)
        if which == 0:
            result = t.call(ALLOWED, b.flow, target, stat, causer, stages, unk69, unk1d)
        elif which == 1:
            result = t.call(CAN_CHANGE, b.flow, causer, target, stat, stages, unk69, unk3, unk7, unk1d,
                            flags[0], flags[1], flags[2])
        else:
            result = t.call(CHANGE, b.flow, causer, target, stat, stages, unk69, unk3, unk7, unk1d,
                            flags[0], flags[3], r.choice([0, b.message]), flags[1], flags[4])
        t.check(result & U8)
        t.check(t.emu.read(b.flow + 0x1511, 1))
        t.check(t.emu.read(b.flow + FAILURES, 0x1C))
        t.check(t.emu.read(VARS, 0x5A4))
