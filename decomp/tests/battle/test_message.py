"""decomp/src/battle/message.cpp against the original: made-up messages of every kind, count and flag, put
through the four putters (faked, recording what they get)."""
import struct

PUT_BATTLE_MESSAGE = 0x71009FA760
# faked: the putters
PUT_ARGS, PUT_ARGS_VALUE, PUT_POKEMON_ARGS, PUT_POKEMON_ARGS_VALUE = (
    0x7100A06E20, 0x7100A08D80, 0x7100A08E60, 0x7100A08F10)
U8, U32 = 0xFF, 0xFFFFFFFF
STATE = {}


def setup(emu):
    def putter(name, value):
        def put(e):
            if value:
                STATE["t"].check((name, e.reg(0), e.reg(1) & U32, e.reg(2) & U32, e.reg(3) & U32, e.reg(4)))
            else:
                STATE["t"].check((name, e.reg(0), e.reg(1) & U32, e.reg(2) & U32, e.reg(3)))
        return put
    emu.stub_function(PUT_ARGS, "PutMessageArgs", putter("PutMessageArgs", False))
    emu.stub_function(PUT_ARGS_VALUE, "PutMessageArgsWithValue", putter("PutMessageArgsWithValue", True))
    emu.stub_function(PUT_POKEMON_ARGS, "PutPokemonMessageArgs", putter("PutPokemonMessageArgs", False))
    emu.stub_function(PUT_POKEMON_ARGS_VALUE, "PutPokemonMessageArgsWithValue",
                      putter("PutPokemonMessageArgsWithValue", True))


def test_put_battle_message(t):
    r = t.rng
    STATE.clear()
    STATE["t"] = t
    flow, message = t.emu.alloc(0x100), t.emu.alloc(0x24)
    for _ in range(3000):
        kind = r.choice([0, 1, 1, 2, 2, 3, 0x41, 0x7F, r.randrange(0x80)])
        info = kind | r.randrange(0x80) << 7 | r.choice([0, 0x4000]) | r.choice([0, 0x8000])
        data = bytearray(r.getrandbits(8) for _ in range(0x24))
        struct.pack_into("<HH", data, 0, r.choice([0, 1, 0x24, 0x118, r.getrandbits(16)]), info)
        t.emu.write(message, bytes(data))
        t.check(t.call(PUT_BATTLE_MESSAGE, flow, message) & U8)
