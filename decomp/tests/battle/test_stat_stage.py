"""decomp/src/battle/stat_change.cpp's stage change against the original: the commands and messages for
made-up Pokémon whose stages rise or fall (by made-up amounts), with every flag."""
import struct

APPLY = 0x71009FA4B0
# faked: the rest of the battle
GET_ID, RAISE, LOWER, PUT, PUT_MESSAGE = 0x71009E2F30, 0x71009E5680, 0x71009E5760, 0x7100A11D70, 0x7100A12B40
U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
STATE = {}                    # the made-up battle, set by the test


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def get_id(e):
        record("GetId", e.reg(0))
        return STATE["id"]

    def stage(name):
        def change(e):
            record(name, e.reg(0), e.reg(1) & U32, e.reg(2) & U8)
            return STATE["changed"]
        return change

    def put(e):
        record("Put", e.reg(0), *(e.reg(i) & U32 for i in range(1, 5)))

    def put_message(e):                              # up to the 0xffff0000 that ends them
        args = []
        for i in range(1, 8):
            args.append(e.reg(i) & U32)
            if args[-1] == 0xFFFF0000:
                break
        record("PutMessage", e.reg(0), *args)
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(RAISE, "RaiseStage", stage("RaiseStage"))
    emu.stub_function(LOWER, "LowerStage", stage("LowerStage"))
    emu.stub_function(PUT, "Put", put)
    emu.stub_function(PUT_MESSAGE, "PutMessage", put_message)


def test_apply_stat_stage(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    flow, commands, pokemon = t.emu.alloc(0x100), t.emu.alloc(0x40), t.emu.alloc(0x400)
    t.emu.write(flow + 0x70, struct.pack("<Q", commands))
    for _ in range(3000):
        STATE["id"] = r.choice([0, 1, 2, 3, 5, 0x1F]) | r.choice([0, 0, 0x100, 0xFFFFFF00])
        STATE["changed"] = r.choice([0, 1, 2, 3, 6, 12]) | r.choice([0, 0, 0x100, 0xFFFFFF00])
        stat = r.choice([1, 2, 3, 4, 5, 6, 7, 0, 8, 0x101, 0x1234])
        stages = r.choice([-12, -6, -3, -2, -1, 0, 1, 2, 3, 6, 12, 0x80, -0x80, 0x101]) & U32
        unk4 = r.choice([0, 0, 0x10000, 0x23, 0xFFFF, r.getrandbits(32)])
        t.call(APPLY, flow, pokemon, stat, stages, unk4, r.choice([0, 1]), r.choice([0, 1]))
