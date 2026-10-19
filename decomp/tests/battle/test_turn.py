"""decomp/src/battle/turn.cpp against the original: the turn's loop, on made-up flows (the actions' count, where
the loop starts, the state, putting the actions in order again, what each action, the end of the turn and the
checks around them give and change made up)."""
import struct

RUN_ACTIONS = 0x71009F1530
# faked: everything the loop calls
ORDER_ACTIONS, RUN_ACTION, AFTER_ACTION, IS_SIDE_OUT = 0x71009F3080, 0x71009F3510, 0x71009F28C0, 0x71009F2D80
GET_UNK168, ENDS_WHEN_SIDE_OUT, END_OF_TURN = 0x71009D7110, 0x7100A54DD0, 0x71009F36A0
CHECK_FIRST, CHECK_SECOND, LIST_CLIENTS = 0x7100A65F50, 0x7100A66080, 0x71009F2C00
GET_PARTY, PUT, FILL_PARTY = 0x71009E0D60, 0x7100A11D70, 0x71009DAB50
U8, U32 = 0xFF, 0xFFFFFFFF
STATE, COUNT, REORDER_FROM, REORDER, EMPTY = 0x84, 0x1508, 0x150A, 0x150B, 0x14F4
CASE = {}                     # the case running, set by each one
BOOLS = [0, 0, 0, 1, 1, 0x100, 0x101]


def setup(emu):
    def record(name, *args):
        CASE["t"].check((name,) + args)

    def pick(values):
        return CASE["t"].rng.choice(values)

    def write(e, offset, fmt, value):
        e.write(CASE["flow"] + offset, struct.pack(fmt, value))

    def order_actions(e):
        record("OrderActions", e.reg(0), e.reg(1), e.reg(2) & U8, e.reg(3) & U8, e.reg(4) & U8)
        return 0

    def run_action(e):
        record("RunAction", e.reg(0), e.reg(1))
        r = CASE["t"].rng
        if r.random() < 0.15:
            write(e, STATE, "<I", pick([0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0x100]))
        if r.random() < 0.1:
            write(e, COUNT, "<B", pick([0, 1, 2, 3, 5, 8, 255]))
        if r.random() < 0.1:
            write(e, REORDER, "<B", pick([0, 1, 0x80]))
            write(e, REORDER_FROM, "<B", r.getrandbits(3))
        return pick([0, 1, r.getrandbits(32)])

    def after_action(e):
        record("AfterAction", e.reg(0))
        return pick(BOOLS + [0, 0, 0])

    def is_side_out(e):
        record("IsSideOut", e.reg(0))
        return pick(BOOLS + [0, 0, 0, 0])

    def get_unk168(e):
        record("GetUnk168", e.reg(0))
        return CASE["t"].rng.getrandbits(32)

    def ends_when_side_out(e):
        record("EndsWhenSideOut", e.reg(0) & U32)
        return pick(BOOLS)

    def end_of_turn(e):
        record("EndOfTurn", e.reg(0))
        return pick([0, 0, 0, 1, 2, 3, 4, 0xFFFFFFFF, 0x100000002, 0x100000000])

    def check_first(e):
        record("CheckFirst", e.reg(0), e.reg(1) & U32)
        return pick([0, 0, 1, 2, 0x100, 0xFF])

    def check_second(e):
        record("CheckSecond", e.reg(0), e.reg(1))
        return pick(BOOLS)

    def list_clients(e):
        record("ListClients", e.reg(0), e.reg(1))
        r = CASE["t"].rng
        count = pick([0, 1, 1, 2, 3, 4])
        e.write(e.reg(1), bytes([count] + [r.getrandbits(2) for _ in range(4)]))
        return pick(BOOLS)

    def get_party(e):
        record("GetParty", e.reg(0), e.reg(1) & U32)
        return 0x5000 + (e.reg(1) & U32) * 0x38

    def put(e):
        record("Put", e.reg(0), e.reg(1) & U32, e.reg(2) & U32)
        return 0

    def fill_party(e):
        record("FillParty", e.reg(0), e.reg(1), e.reg(2) & U8)
        return 0
    for addr, name, fn in [(ORDER_ACTIONS, "OrderActions", order_actions), (RUN_ACTION, "RunAction", run_action),
                           (AFTER_ACTION, "AfterAction", after_action), (IS_SIDE_OUT, "IsSideOut", is_side_out),
                           (GET_UNK168, "GetUnk168", get_unk168),
                           (ENDS_WHEN_SIDE_OUT, "EndsWhenSideOut", ends_when_side_out),
                           (END_OF_TURN, "EndOfTurn", end_of_turn), (CHECK_FIRST, "CheckFirst", check_first),
                           (CHECK_SECOND, "CheckSecond", check_second), (LIST_CLIENTS, "ListClients", list_clients),
                           (GET_PARTY, "GetParty", get_party), (PUT, "Put", put),
                           (FILL_PARTY, "FillParty", fill_party)]:
        emu.stub_function(addr, name, fn)


def new_flow(t):
    """A made-up flow (made once per emulator, filled anew): random bytes, the setup, the Pokémon list and the
    server commands as plain addresses, a state, an action count and the reordering."""
    e, r = t.emu, t.rng
    if not hasattr(e, "turn_flow"):
        e.turn_flow = e.alloc(0x2000)
    flow = e.turn_flow
    e.write(flow, bytes(r.getrandbits(8) for _ in range(0x2000)))
    e.write(flow + 0x60, struct.pack("<QQQ", 0x1000, 0x2000, 0x3000))
    e.write(flow + STATE, struct.pack("<I", r.choice([0, 0, 0, 0, 1, 2, 3, 4, 6, r.getrandbits(32)])))
    e.write(flow + COUNT, bytes([r.choice([0, 1, 2, 3, 4, 6, 25, 255, r.getrandbits(8)])]))
    e.write(flow + REORDER_FROM, bytes([r.choice([0, 1, 2, 3, r.getrandbits(8)])]))
    e.write(flow + REORDER, bytes([r.choice([0, 0, 1, 0x80])]))
    CASE.clear()
    CASE["t"], CASE["flow"] = t, flow
    return flow


def test_turn(t):
    r = t.rng
    for _ in range(4000):
        flow = new_flow(t)
        count = t.emu.read(flow + COUNT, 1)[0]
        start = r.choice([0, 0, 0, 1, 2, max(count - 1, 0), count, count + 1, r.getrandbits(8), r.getrandbits(32),
                          0xFFFFFFFF])
        t.check(t.call(RUN_ACTIONS, flow, start) & U32)
        t.check(t.emu.read(flow + STATE, 4))
        t.check(t.emu.read(flow + EMPTY, 5))
