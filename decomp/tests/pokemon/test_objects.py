"""The Pokémon objects' construction against the originals (decomp/src/pokemon/pokemon.cpp,
party_pokemon.cpp, evolution.cpp): Pokémon and party Pokémon made each way, copied and destroyed,
eggs, and the evolution checker, all in a fake resource (pkm.FakeResource)."""
import struct

import pkm
import test_pokemon as tp

POKEMON_INITIALIZE, MAKE_EGG = 0x7100723770, 0x7100728020
POKEMON_CTORS = {"blank": 0x7100723780, "new": 0x71007239E0, "spec": 0x7100723E00, "copy": 0x71007243A0}
POKEMON_DTORS = [(0x7100724600, False), (0x7100724740, True), (0x7100724750, False), (0x7100724760, True)]
PARTY_INITIALIZE, PARTY_COPY_FROM = 0x71007291A0, 0x7100729440
PARTY_SERIALIZE_CORE, PARTY_DESERIALIZE_CORE = 0x71007294E0, 0x71007294F0
PARTY_CTORS = {"blank": 0x71007291B0, "new": 0x71007292D0, "spec": 0x7100729350, "copy": 0x71007293D0}
PARTY_DTORS = [(0x7100729480, False), (0x7100729490, True), (0x71007294A0, False), (0x71007294B0, True)]
CHECKER_CTOR, BLOCKS_EVOLUTION = 0x7100722960, 0x7100723760
CHECKER_DTORS = [(0x71007229F0, False), (0x7100722A00, True), (0x7100722A10, False), (0x7100722A20, True)]
OBJECT_DTORS = [0x71000030D0, 0x71000030E0]                 # gfl::Object's, which ours stand in for
SCRATCH, SCRATCH_SIZE = 0x710163ECD8, pkm.CORE_SIZE + pkm.CORE_SIZE + pkm.CALC_SIZE   # g_scratchCore, g_scratchRecord
KINDS = list(POKEMON_CTORS)
SYS = [None]


def setup(emu):
    tp.setup(emu)
    pkm.fake_evolutions(emu)
    SYS[0] = pkm.FakeSystem(emu, tp.SYSTEM[0])


def readable(t, ptr, size):
    """The memory at ptr, or the pointer itself when it doesn't point at any."""
    try:
        return t.emu.read(ptr, size) if ptr else ptr
    except Exception:
        return ptr


def snapshot(t, obj, res):
    """The object, the records and the accessor it owns, the resource's log and stack, the scratch
    records and the default records."""
    e = t.emu
    owned = [readable(t, e.read_u64(obj + off), size)
             for off, size in ((0x58, pkm.CORE_SIZE), (0x78, pkm.CALC_SIZE), (0x98, 0x20))]
    return (e.read(obj, 0xB8), owned, res.state(), e.read(SCRATCH, SCRATCH_SIZE), e.read(pkm.DEFAULTS, pkm.DEFAULTS_SIZE))


def set_current_resource(t, res):
    """The resource stack as construction finds it: empty, or with this or another resource on top."""
    entries = [t.rng.choice([res.addr, 0x7101234560]) for _ in range(16)]
    t.emu.write(res.stack, struct.pack("<17Q", *entries, t.rng.choice([0, 1, 1, 2])))


def other_pokemon(t, res, party):
    """A Pokémon to copy: a random record in any state (test_pokemon's Mon), or one made here. Party
    Pokémon copy the whole record, so theirs has party stats (the game reads them regardless)."""
    if t.rng.random() < 0.5:
        return tp.Mon(t, *t.rng.choice([s for s in pkm.STATES if s[1] or not party])).obj
    return make(t, res, t.rng.choice(KINDS[:3]), party=party or t.rng.random() < 0.5)


def make(t, res, kind, party=False):
    obj = t.emu.alloc(b"\xcc" * 0xB8)
    ctor = (PARTY_CTORS if party else POKEMON_CTORS)[kind]
    SYS[0].randomize(t.rng)
    if kind == "copy":
        other = other_pokemon(t, res, party)
    set_current_resource(t, res)
    if kind == "blank":
        t.call(ctor, obj, res.addr)
    elif kind == "new":
        level = t.rng.choice([1, 5, 50, 100, 150])
        t.call(ctor, obj, res.addr, t.rng.randrange(1, 900), level, t.rng.choice([pkm.RANDOM, t.rng.getrandbits(32)]))
    elif kind == "spec":
        t.call(ctor, obj, res.addr, t.emu.alloc(pkm.random_spec(t.rng)))
    else:
        t.call(ctor, obj, res.addr, other)
    return obj


def test_pokemon_construction(t):
    res = pkm.FakeResource(t.emu)
    for kind in KINDS * 10:
        t.check(snapshot(t, make(t, res, kind), res))


def test_party_pokemon_construction(t):
    res = pkm.FakeResource(t.emu)
    for kind in KINDS * 10:
        t.check(snapshot(t, make(t, res, kind, party=True), res))


def test_destruction(t):
    res = pkm.FakeResource(t.emu)
    for party, dtors in ((False, POKEMON_DTORS), (True, PARTY_DTORS)):
        for dtor, thunk in dtors:
            for kind in KINDS:
                obj = make(t, res, kind, party)
                if t.rng.random() < 0.3:                  # an owner can be empty
                    t.emu.write(obj + 0x58 + 0x20 * t.rng.randrange(3), struct.pack("<Q", 0))
                t.call(dtor, obj + 0x48 if thunk else obj)
                t.check(snapshot(t, obj, res))


def test_party_pokemon_methods(t):
    res = pkm.FakeResource(t.emu)
    for _ in range(40):
        obj = make(t, res, t.rng.choice(KINDS), party=True)
        which = t.rng.randrange(4)
        if which == 0:                                    # virtual: recalculates and heals
            core = t.emu.alloc(pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.2)[2])
            t.call(PARTY_DESERIALIZE_CORE, obj, core)
        elif which == 1:
            out = t.emu.alloc(pkm.CORE_SIZE)
            t.call(PARTY_SERIALIZE_CORE, obj, out)
            t.check(t.emu.read(out, pkm.CORE_SIZE))
        elif which == 2:
            t.call(PARTY_COPY_FROM, obj, other_pokemon(t, res, party=True))
        else:
            t.call(POKEMON_INITIALIZE)
            t.call(PARTY_INITIALIZE)
        t.check(snapshot(t, obj, res))


def test_make_egg(t):
    res = pkm.FakeResource(t.emu)
    for _ in range(30):
        if t.rng.random() < 0.3:                          # any record, bad eggs included
            mon = tp.Mon(t, *t.rng.choice(pkm.STATES))
            obj, look = mon.obj, mon.rec.snapshot
        else:
            obj = make(t, res, t.rng.choice(KINDS), party=t.rng.random() < 0.5)
            look = lambda: snapshot(t, obj, res)
        SYS[0].randomize(t.rng)
        set_current_resource(t, res)
        t.call(MAKE_EGG, obj, res.addr)
        t.check(look())
        t.check(res.state())


def test_evolution_checker(t):
    res = pkm.FakeResource(t.emu)
    for _ in range(4):
        set_current_resource(t, res)
        obj = t.emu.alloc(b"\xcc" * 0x58)
        t.call(CHECKER_CTOR, obj)
        t.check(t.emu.read(obj, 0x58))
        for item in list(range(0, 1000, 37)) + [0xFFFF]:
            t.check(t.call(BLOCKS_EVOLUTION, obj, item) & 0xFF)
        for dtor, thunk in CHECKER_DTORS:
            t.call(dtor, obj + 0x48 if thunk else obj)
            t.check(t.emu.read(obj, 0x58))
        for dtor in OBJECT_DTORS:
            t.call(dtor, obj)
            t.check(t.emu.read(obj, 0x58))
