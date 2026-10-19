"""decomp/src/battle/add_condition.cpp's AddCondition and calc.cpp's status check against the originals:
conditions given to made-up Pokémon (a Shaymin in either form among them), with or without their message or
another one, every follow-up faked and recorded, and made-up handlers on the events it calls."""
import random
import struct

IS_STATUS, ADD_CONDITION = 0x71009D00E0, 0x71009FF450
RESET, ADD = 0x7100A66740, 0x7100A667F0
# faked: the rest of the battle
GET_ID, GET_VALUE, GET_SPECIES, CHANGE_FORM, GET_POKEMON = (
    0x71009E2F30, 0x71009E3690, 0x71009E3670, 0x71009E6B50, 0x7100A095E0)
SET_CONDITION, MAKE_MESSAGE, INDEX_OF, PUT, PUT_MESSAGE = (
    0x71009FEDF0, 0x7100A14EF0, 0x7100A6B7A0, 0x7100A11D70, 0x7100A12B40)
SAVE, RESTORE, STATUS_ADDED, UNNERVE, CONDITION_ADDED, ITEM_REACTION = (
    0x7100A68E80, 0x7100A68EE0, 0x7100A05450, 0x7100A054F0, 0x7100A05580, 0x7100A01D00)
REMOVE_ABILITY, ADD_ABILITY = 0x7100A2A770, 0x7100A28C70

VARS = 0x710164D770
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
MESSAGE = 0x2720                                 # Flow::message
STATE = {}                    # the made-up battle, set by each test


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def pokemon(e):
        return STATE["pokemon"].get(e.reg(0), {})

    def get_id(e):
        record("GetId", e.reg(0))
        return pokemon(e).get("id", 0x1F)

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get("ability", 0)

    def get_species(e):
        record("GetSpecies", e.reg(0))
        return pokemon(e).get("species", 0)

    def simple(name, n):
        def run(e):
            record(name, *(e.reg(i) & (U32 if i else 0xFFFFFFFFFFFFFFFF) for i in range(n)))
        return run

    def set_condition(e):
        record("SetCondition", e.reg(0), e.reg(1), e.reg(2) & U32, e.read(e.reg(3), 8))

    def make_message(e):
        record("MakeConditionMessage", e.reg(0) & U32, e.read(e.reg(1), 8), e.reg(2), e.reg(3))
        data = bytearray(STATE["message"])
        e.write(e.reg(3), bytes(data))

    def index_of(e):
        record("IndexOf", e.reg(0), e.reg(1) & U8)
        return STATE["index"]

    def put(e):
        record("Put", e.reg(0), *(e.reg(i) & U32 for i in range(1, 5)))

    def put_message(e):                              # up to the 0xffff0000 that ends them
        args = []
        for i in range(1, 8):
            args.append(e.reg(i) & U32)
            if args[-1] == 0xFFFF0000:
                break
        record("PutMessage", e.reg(0), *args)

    def save(e):
        record("Save", e.reg(0))
        return STATE["saved"]

    def status_added(e):
        record("StatusAdded", e.reg(0), e.reg(1), e.reg(2), e.reg(3) & U32, e.reg(4))
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(GET_SPECIES, "GetSpecies", get_species)
    emu.stub_function(CHANGE_FORM, "ChangeForm", simple("ChangeForm", 3))
    emu.stub_function(SET_CONDITION, "SetCondition", set_condition)
    emu.stub_function(MAKE_MESSAGE, "MakeConditionMessage", make_message)
    emu.stub_function(INDEX_OF, "IndexOf", index_of)
    emu.stub_function(PUT, "Put", put)
    emu.stub_function(PUT_MESSAGE, "PutMessage", put_message)
    emu.stub_function(SAVE, "Save", save)
    emu.stub_function(RESTORE, "Restore", simple("Restore", 2))
    emu.stub_function(STATUS_ADDED, "StatusAdded", status_added)
    emu.stub_function(UNNERVE, "UnnerveSuppressed", simple("UnnerveSuppressed", 2))
    emu.stub_function(CONDITION_ADDED, "ConditionAdded", simple("ConditionAdded", 4))
    emu.stub_function(ITEM_REACTION, "CallItemReaction", simple("CallItemReaction", 3))
    emu.stub_function(REMOVE_ABILITY, "RemoveAbilityHandlers", simple("RemoveAbilityHandlers", 1))
    emu.stub_function(ADD_ABILITY, "AddAbilityHandlers", simple("AddAbilityHandlers", 1))
    emu.stub_function(GET_POKEMON, "GetPokemon", lambda e: 0)        # skip checks: no owner


def test_is_status_condition(t):
    STATE.clear()
    STATE["t"] = t
    for c in list(range(0x30)) + [0xFF, 0x105, 0x7FFFFFFF, 0x80000000, 0x80000005, U32, U32 - 6]:
        t.check(t.call(IS_STATUS, c) & U8)


class Battle:
    """A made-up battle: a flow object with its message, two Pokémon, and handlers on the events AddCondition
    calls (they only record what they see)."""

    EVENTS = [0x81, 0xCC]

    def __init__(self, t):
        self.t, self.e, self.r = t, t.emu, t.rng
        e, r = self.e, self.r
        STATE.clear()
        STATE["t"] = t
        STATE["pokemon"] = {}
        sizes = (0x2900, 0x100, 0x200, 0x40, 0x300, 0x400, 0x400, 0x24, 0x8)
        if not hasattr(e, "scene"):                 # the big objects, made once per test (the heap is 16 MB)
            e.scene = [e.alloc(n) for n in sizes]
        self.flow, setup_obj, mons, commands, field, self.target, self.attacker, self.other, self.data = e.scene
        for addr, n in zip(e.scene, sizes):
            e.write(addr, bytes(n))
        for off, v in ((0x60, setup_obj), (0x68, mons), (0x70, commands), (0x88, field)):
            e.write(self.flow + off, struct.pack("<Q", v))
        e.write(self.flow + MESSAGE, bytes(r.getrandbits(8) for _ in range(0x24)))
        e.write(self.data, struct.pack("<Q", r.getrandbits(64)))
        message = bytearray(r.getrandbits(8) for _ in range(0x24))
        struct.pack_into("<H", message, 2, r.choice([0, 1, 2, 3]) | r.randrange(8) << 7 | r.choice([0, 0x4000]))
        STATE["message"] = bytes(message)
        other = bytearray(r.getrandbits(8) for _ in range(0x24))
        struct.pack_into("<H", other, 2, r.choice([0, 1, 2]) | r.randrange(8) << 7 | r.choice([0, 0x4000]))
        e.write(self.other, bytes(other))
        STATE["index"] = r.choice([0, 1, 2, 3, 4, 4]) | r.choice([0, 0, 0x100, 0xFFFFFF00])
        STATE["saved"] = r.getrandbits(32)
        for i, p in enumerate((self.target, self.attacker)):
            STATE["pokemon"][p] = {"id": r.choice([i, i, 5, 0x101]), "species": r.choice([492, 492, 25, 0x101EC]),
                                   "ability": r.choice([127, 127, 0, 126, 0x17F])}
            e.write(p + 0x2E1, bytes([r.choice([0, 1, 1, 2, 0x81])]))
        t.call(RESET)
        STATE["battle"] = self
        if not hasattr(e, "handlers"):              # made once per test: the emulator has room for few
            e.handlers = {ev: e.host_function("on%x" % ev, Battle.handler(ev)) for ev in self.EVENTS}
            e.tables = {ev: e.alloc(struct.pack("<IIQ", ev, 0, e.handlers[ev])) for ev in self.EVENTS}
        for ev in self.EVENTS:
            if r.random() < 0.6:
                t.call(ADD, r.choice([0, 1, 2, 3, 4, 5]), r.getrandbits(16), r.randrange(10), r.randrange(8),
                       r.choice([0, 1, 2]), e.tables[ev], 1)

    @staticmethod
    def handler(event):
        def run(e):
            self = STATE["battle"]
            self.t.check(("event", event, e.reg(2) & U8, self.e.read(VARS, 0x5A4)))
        return run


def test_add_condition(t):
    r = t.rng
    for _ in range(2500):
        b = Battle(t)
        condition = r.choice([0, 1, 2, 3, 3, 3, 4, 5, 6, 7, 8, 0x10, 0x10, 0x12, 0x17, 0xFFFF, 0x80000001, 0x103])
        t.call(ADD_CONDITION, b.flow, b.target, b.attacker, condition, b.data, r.choice([0, 1]), r.choice([0, 1]),
               r.choice([0, b.other]))
        t.check(t.emu.read(b.flow + MESSAGE, 0x24))
        t.check(t.emu.read(VARS, 0x5A4))
