"""decomp/src/pokemon/accessor.cpp against the originals: every state an accessor can be in
(encrypted, fast mode, decrypted; with and without party stats; bad eggs)."""
import struct

import pkm

CTOR, DTOR, DTOR_DELETING = 0x7100729540, 0x7100729560, 0x7100729570
ATTACH_DECODED, ATTACH_ENCODED, HAS_CALC = 0x7100729580, 0x7100729600, 0x7100729610
CLEAR, CLEAR_CALC = 0x7100729620, 0x71007296D0
IS_FAST, START_FAST, END_FAST = 0x71007297F0, 0x7100729800, 0x7100729890
SERIALIZE, SERIALIZE2, SERIALIZE_CORE = 0x7100729920, 0x7100729930, 0x7100729A70
DESERIALIZE, DESERIALIZE2, DESERIALIZE_CORE = 0x7100729A80, 0x7100729A90, 0x7100729BC0
GET_KEY, IS_BAD_EGG = 0x7100729BD0, 0x7100729BE0


def setup(emu):
    emu.call(pkm.INIT_DEFAULT_RECORDS)
    emu.stub_function(pkm.OPERATOR_DELETE, "operator delete", lambda e: None)


def each_state(t, rounds=4):
    for _ in range(rounds):
        for state, calc, bad in pkm.STATES:
            yield pkm.Record(t, state, calc, bad)


def test_construct_destroy(t):
    obj = t.emu.alloc(b"\xcc" * 0x20)
    t.call(CTOR, obj)
    t.check(t.emu.read(obj, 0x20))          # the vtable is the original's (decomp/symbols.txt)
    t.call(DTOR, obj)
    t.call(DTOR_DELETING, obj)
    t.check(t.emu.read(obj, 0x20))


def test_attach(t):
    for r in each_state(t):
        core, calc, core_enc, calc_enc = pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.3)
        c, k = t.emu.alloc(core), t.emu.alloc(calc)
        t.call(ATTACH_DECODED, r.obj, c, k if t.rng.random() < 0.7 else 0)
        t.check((r.snapshot(), t.emu.read(c, pkm.CORE_SIZE), t.emu.read(k, pkm.CALC_SIZE)))
        c, k = t.emu.alloc(core_enc), t.emu.alloc(calc_enc)
        t.call(ATTACH_ENCODED, r.obj, c, k)
        t.check(t.call(HAS_CALC, r.obj) & 1)
        t.check(r.snapshot())


def test_clear(t):
    for r in each_state(t):
        t.call(CLEAR_CALC, r.obj)
        t.check(r.snapshot())
        t.call(CLEAR, r.obj)
        t.check(r.snapshot())


def test_fast_mode(t):
    for r in each_state(t):
        t.check(t.call(IS_FAST, r.obj) & 1)
        t.call(START_FAST, r.obj)
        t.check((r.snapshot(), t.call(IS_FAST, r.obj) & 1, t.call(IS_BAD_EGG, r.obj) & 1))
        t.call(START_FAST, r.obj)
        t.check(r.snapshot())
        t.call(END_FAST, r.obj)
        t.check((r.snapshot(), t.call(GET_KEY, r.obj) & 0xFFFFFFFF))
        t.call(END_FAST, r.obj)
        t.check(r.snapshot())


def test_serialize(t):
    # (copying party stats in or out of a Pokémon without them reads null: the game never does)
    for r in each_state(t):
        out = t.emu.alloc(b"\xcc" * 0x110)
        if r.calc:
            t.call(SERIALIZE, r.obj, out)
            t.check((t.emu.read(out, 0x110), r.snapshot()))
        out = t.emu.alloc(b"\xcc" * 0x110)
        t.call(SERIALIZE_CORE, r.obj, out)
        t.check((t.emu.read(out, 0x110), r.snapshot()))
        core, calc = t.emu.alloc(b"\xcc" * 0xF0), t.emu.alloc(b"\xcc" * 0x20)
        t.call(SERIALIZE2, r.obj, core if t.rng.random() < 0.8 else 0, calc if r.calc and t.rng.random() < 0.8 else 0)
        t.check((t.emu.read(core, 0xF0), t.emu.read(calc, 0x20), r.snapshot()))


def test_deserialize(t):
    for r in each_state(t):
        _core, _calc, core_enc, calc_enc = pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.3)
        if r.calc:
            t.call(DESERIALIZE, r.obj, t.emu.alloc(core_enc + calc_enc))
            t.check((r.snapshot(), t.call(IS_BAD_EGG, r.obj) & 1))
        _core, _calc, core_enc, calc_enc = pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.3)
        t.call(DESERIALIZE_CORE, r.obj, t.emu.alloc(core_enc))
        t.check(r.snapshot())
        _core, _calc, core_enc, calc_enc = pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.3)
        c = t.emu.alloc(core_enc) if t.rng.random() < 0.8 else 0
        k = t.emu.alloc(calc_enc) if r.calc and t.rng.random() < 0.8 else 0
        t.call(DESERIALIZE2, r.obj, c, k)
        t.check(r.snapshot())
