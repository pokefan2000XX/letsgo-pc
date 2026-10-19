"""decomp/src/pokemon/record.cpp's new records against the originals: specs with every kind of field
left to chance or set (pkm.random_spec), rolled with the system's random numbers (pkm.FakeSystem)."""
import struct

import pkm
import test_pokemon as tp

CREATE_FOR_SPECIES, CREATE_FROM_SPEC = 0x7100739640, 0x71007396B0
RESOLVE_SPEC, INIT_CORE_DATA, ROLL_SIZE = 0x7100739820, 0x7100739CD0, 0x7100739ED0
SYS = [None]


def setup(emu):
    tp.setup(emu)
    pkm.fake_evolutions(emu)
    SYS[0] = pkm.FakeSystem(emu, tp.SYSTEM[0])


def test_roll_size(t):
    values = list(range(0x110)) + [0x7FFF, 0x8000, 0xFFFE, 0xFFFF] + [t.rng.getrandbits(16) for _ in range(100)]
    for value in values:
        for _ in range(4 if 0x100 <= value <= 0x105 else 1):
            t.check(t.call(ROLL_SIZE, value) & 0xFFFF)


def test_resolve_spec(t):
    spec, out = t.emu.alloc(0x40), t.emu.alloc(0x40)
    for _ in range(500):
        data = pkm.random_spec(t.rng)
        SYS[0].shiny_id = pkm.SPEC.unpack(data)[2] & 0xFFFFFFFF if t.rng.random() < 0.5 else None
        t.emu.write(spec, data)
        t.emu.write(out, b"\xcc" * 0x40)
        t.call(RESOLVE_SPEC, out, spec)
        t.check((t.emu.read(out, 0x40), t.emu.read(spec, 0x40)))


def test_init_core_data(t):
    core, spec = t.emu.alloc(pkm.CORE_SIZE), t.emu.alloc(0x40)
    for _ in range(200):
        SYS[0].randomize(t.rng)
        t.emu.write(spec, pkm.random_spec(t.rng, species=0 if t.rng.random() < 0.1 else None))
        start = bytes(t.rng.randrange(256) for _ in range(pkm.CORE_SIZE)) if t.rng.random() < 0.3 else bytes(pkm.CORE_SIZE)
        t.emu.write(core, start)
        t.call(INIT_CORE_DATA, core, spec)
        t.check(t.emu.read(core, pkm.CORE_SIZE))


def test_create_core_data(t):
    res = pkm.FakeResource(t.emu)
    out = t.emu.alloc(0x20)
    for _ in range(80):
        SYS[0].randomize(t.rng)
        if t.rng.random() < 0.5:
            t.call(CREATE_FROM_SPEC, res.addr, t.emu.alloc(pkm.random_spec(t.rng)), x8=out)
        else:
            level = t.rng.choice([1, 5, 50, 100, 101, t.rng.getrandbits(16)])
            trainer = t.rng.choice([pkm.RANDOM, t.rng.getrandbits(32), t.rng.getrandbits(64)])
            t.call(CREATE_FOR_SPECIES, res.addr, t.rng.randrange(900), level, trainer, x8=out)
        ptr = struct.unpack("<Q", t.emu.read(out, 8))[0]
        t.check((t.emu.read(out, 0x20), t.emu.read(ptr, pkm.CORE_SIZE)))
    t.check(res.state())
