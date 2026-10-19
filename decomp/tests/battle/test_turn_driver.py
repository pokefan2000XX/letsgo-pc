"""decomp/src/battle/turn.cpp's RunTurn against the original: running a turn or going on with it, on made-up
flows (whether the turn has started, where the loop goes on from, the state, the turn limit and what the loop and
the end of the turn give and change made up)."""
import struct

RUN_TURN = 0x71009F10A0
# faked: everything it calls, the turn's loop too
CALLS = {0x7100A510F0: ("Ready1", 1), 0x7100A65970: ("Ready2", 1), 0x7100A667C0: ("WakeAllHandlerSets", 0),
         0x71009EEB60: ("Ready3", 1), 0x7100A66F70: ("ResetVars", 0), 0x71009F11D0: ("Ready4", 1),
         0x71009F12B0: ("Ready5", 1), 0x71009F1340: ("MakeActions", 6), 0x71009F1490: ("AfterMakeActions", 1)}
RUN_ACTIONS, END_TURN, TURN_LIMIT = 0x71009F1530, 0x71009F1760, 0x71009D8D40
U8, U32 = 0xFF, 0xFFFFFFFF
STATE, UNK80, STARTED, NEXT, UNK2AD5 = 0x84, 0x80, 0x1504, 0x1509, 0x2AD5
CASE = {}                     # the case running, set by each one


def setup(emu):
    def record(name, *args):
        CASE["t"].check((name,) + args)

    def plain(name, count):
        def handler(e):
            record(name, *[e.reg(i) for i in range(count)])
            return CASE["t"].rng.getrandbits(64)
        return handler

    def run_actions(e):
        record("RunActions", e.reg(0), e.reg(1) & U32)
        r = CASE["t"].rng
        if r.random() < 0.6:
            e.write(CASE["flow"] + STATE, struct.pack("<I", r.choice([0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x100,
                                                                       0x80000000, r.getrandbits(32)])))
        return r.choice([0, 1, 5, 0x100, 0x1FF, r.getrandbits(64)])

    def end_turn(e):
        record("EndTurn", e.reg(0))
        r = CASE["t"].rng
        if r.random() < 0.3:
            e.write(CASE["flow"] + STATE, struct.pack("<I", r.choice([0, 1, 4, 7, r.getrandbits(32)])))
        return r.getrandbits(64)

    def turn_limit(e):
        record("TurnLimit", e.reg(0))
        r = CASE["t"].rng
        return r.choice([0, 0, 1, 2, 5, 0xFF, 0x100, 0x105, r.getrandbits(64)])
    for addr, (name, count) in CALLS.items():
        emu.stub_function(addr, name, plain(name, count))
    emu.stub_function(RUN_ACTIONS, "RunActions", run_actions)
    emu.stub_function(END_TURN, "EndTurn", end_turn)
    emu.stub_function(TURN_LIMIT, "TurnLimit", turn_limit)


def test_turn_driver(t):
    e, r = t.emu, t.rng
    flow, commands = e.alloc(0x3000), e.alloc(0x40)
    for _ in range(4000):
        e.write(flow, r.randbytes(0x3000))
        e.write(commands, r.randbytes(0x40))
        e.write(flow + 0x60, struct.pack("<QQQ", 0x1000, 0x2000, commands))
        e.write(flow + STATE, struct.pack("<I", r.choice([0, 1, 4, r.getrandbits(32)])))
        e.write(flow + UNK80, struct.pack("<I", r.choice([0, 1, 2, 4, 5, 6, 0xFF, 0x100, r.getrandbits(32)])))
        e.write(flow + STARTED, bytes([r.choice([0, 0, 1, 0x80, 0xFE])]))
        CASE.clear()
        CASE["t"], CASE["flow"] = t, flow
        choices = r.getrandbits(64)
        t.check(t.call(RUN_TURN, flow, choices) & U32)
        t.check(e.read(flow + STATE, 4))
        t.check(e.read(flow + STARTED, 1))
        t.check(e.read(flow + NEXT, 1))
        t.check(e.read(flow + UNK2AD5, 4))
        t.check(e.read(commands, 8))
