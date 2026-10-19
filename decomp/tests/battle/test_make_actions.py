"""decomp/src/battle/turn.cpp's MakeActions against the original: making the turn's actions, on made-up turns
(how many actions there are, their kinds, which Pokémon each is for and their IDs made up)."""
import struct

MAKE_ACTIONS = 0x71009F1340
# faked: making the actions themselves, putting them in order and the Pokémon's IDs
MAKE, ORDER_ACTIONS, GET_ID = 0x71009F1C30, 0x71009F3080, 0x71009E2F30
U8 = 0xFF
CASE = {}                     # the case running, set by each one


def setup(emu):
    def record(name, *args):
        CASE["t"].check((name,) + args)

    def make(e):
        record("Make", e.reg(0), e.reg(1), e.reg(2))
        r = CASE["t"].rng
        count = r.choice([0, 1, 2, 3, 4, 5, 6, 8, 12, 25])
        for i in range(25):           # every slot, since OrderActions may change the count
            kind = r.choice([0, 1, 1, 1, 2, 3, 4, 5, 5, 6, 6, 9, 0xD])
            e.write(e.reg(2) + i * 0x18, struct.pack("<QI", r.choice(CASE["mons"]), (r.getrandbits(28) << 4) | kind))
            e.write(e.reg(2) + i * 0x18 + 0x17, bytes([r.choice([0, 0, 1, 0x80])]))
        return count | r.choice([0, 0, 0x100, 0xFFFFFF00])

    def order_actions(e):
        record("OrderActions", e.reg(0), e.reg(1), e.reg(2) & U8, e.reg(3) & U8, e.reg(4) & U8)
        r = CASE["t"].rng
        if r.random() < 0.1:
            e.write(CASE["count"], bytes([r.choice([0, 1, 2, 3])]))
        return 0

    def get_id(e):
        return CASE["ids"][e.reg(0)]
    emu.stub_function(MAKE, "Make", make)
    emu.stub_function(ORDER_ACTIONS, "OrderActions", order_actions)
    emu.stub_function(GET_ID, "GetId", get_id)


def test_make_actions(t):
    e, r = t.emu, t.rng
    actions, small = e.alloc(0x300), e.alloc(0x10)
    mons = [0x9000 + i * 0x100 for i in range(6)]
    for _ in range(4000):
        e.write(actions, r.randbytes(0x300))
        e.write(small, r.randbytes(0x10))
        CASE.clear()
        CASE["t"], CASE["count"], CASE["mons"] = t, small, mons
        # IDs: a few Pokémon share one now and then, and the bits above the low byte don't count
        CASE["ids"] = {m: r.choice([0, 1, 2, 3, 4, 5, 0x101, 0x205, r.getrandbits(32)]) for m in mons}
        flow, choices = r.getrandbits(64), r.getrandbits(64)
        t.call(MAKE_ACTIONS, flow, choices, actions, small, small + 1, small + 2)
        t.check(e.read(small, 3))
        t.check(e.read(actions, 0x300))
