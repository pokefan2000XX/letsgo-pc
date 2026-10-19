"""decomp/src/pokemon/party.cpp against the originals: parties made and destroyed in a fake resource,
then changed at random (adding, setting, removing, swapping, reordering, copying, healing) with every
query checked after each change, and saved and loaded with and without party stats."""
import struct

import pkm
import test_objects as to

CTOR, CREATE = 0x7100720D80, 0x7100722840
DTORS = [(0x7100721070, False), (0x7100721140, True), (0x7100721210, False), (0x71007212E0, True)]
ADD, SET, REMOVE, COMPACT = 0x71007213B0, 0x7100721480, 0x7100721550, 0x7100721610
SWAP, REORDER = 0x7100721700, 0x71007217C0
GET_MEMBER, GET_MEMBER_CONST, GET_MEMBER_COUNT, IS_FULL = 0x7100721960, 0x7100721970, 0x7100721990, 0x71007219A0
COUNT, COUNT_SKIPPING, HAS_SPECIES = 0x71007219B0, 0x71007219C0, 0x7100721B20
COPY_FROM, CLEAR, HEAL_ALL = 0x7100721CC0, 0x7100721E40, 0x7100722760
SET_PARTNER, GET_PARTNER, UPDATE_PARTNER = 0x71007227C0, 0x71007227D0, 0x71007227E0
SERIALIZE_CORES, CORES_SIZE, SERIALIZE_WITH, SERIALIZE = 0x7100721EF0, 0x7100721FE0, 0x7100721FF0, 0x71007221A0
DESERIALIZE_CORES, DESERIALIZE_WITH, DESERIALIZE = 0x71007221C0, 0x71007223D0, 0x7100722750
ADD_REF = 0x7100003030                             # gfl::RefObject::AddRef (gflib3)
GET_SPECIES = 0x71007247C0
SAVED_SIZE, RECORD_SIZE = 0x61C, pkm.CORE_SIZE + pkm.CALC_SIZE
HIGH = 0xABCD << 32                                # junk above a 32-bit argument


def setup(emu):
    to.setup(emu)


def member_state(t, obj):
    """A member: the object, and the records and accessor it owns."""
    if not obj:
        return obj
    e = t.emu
    owned = [to.readable(t, e.read_u64(obj + off), size)
             for off, size in ((0x58, pkm.CORE_SIZE), (0x78, pkm.CALC_SIZE), (0x98, 0x20))]
    return e.read(obj, 0xB8), owned


def snapshot(t, party, res):
    """The party, its members, the resource's log and stack, and the scratch and default records."""
    e = t.emu
    members = [e.read_u64(party + 0x58 + 8 * i) for i in range(6)]
    return (e.read(party, 0x90), [member_state(t, m) for m in members], res.state(),
            e.read(to.SCRATCH, to.SCRATCH_SIZE), e.read(pkm.DEFAULTS, pkm.DEFAULTS_SIZE))


def make_party(t, res):
    party = t.emu.alloc(b"\xcc" * 0x90)
    to.SYS[0].randomize(t.rng)
    to.set_current_resource(t, res)
    t.call(CTOR, party, res.addr)
    return party


def pokemon(t, res):
    """A Pokémon to put in: an empty one sometimes, otherwise any with party stats (eggs, bad eggs
    and fainted ones among them)."""
    if t.rng.random() < 0.15:
        return to.make(t, res, "blank", party=True)
    return to.other_pokemon(t, res, party=True)


def fill(t, party, res, count=None):
    """Adds members, some of them empty (which Add refuses)."""
    for _ in range(t.rng.randrange(7) if count is None else count):
        t.check(t.call(ADD, party, pokemon(t, res)) & 0xFF)


def queries(t, party):
    """Everything the party answers, with arguments in and out of range."""
    e = t.emu
    for i in list(range(8)) + [0xFFFFFFFF]:
        arg = i | HIGH if t.rng.random() < 0.3 else i
        t.check((t.call(GET_MEMBER, party, arg), t.call(GET_MEMBER_CONST, party, arg)))
    t.check((t.call(GET_MEMBER_COUNT, party) & 0xFF, t.call(IS_FULL, party) & 0xFF, t.call(GET_PARTNER, party) & 0xFF))
    for kind in list(range(7)) + [0xFFFFFFFF]:
        t.check(t.call(COUNT, party, kind) & 0xFFFFFFFF)
        skipped = t.rng.choice([0, 1, 2, 0x21, 0x3F, 0xFF, t.rng.randrange(256)]) | t.rng.choice([0, 0x100, HIGH])
        t.check(t.call(COUNT_SKIPPING, party, kind, skipped) & 0xFFFFFFFF)
    species = [t.call(GET_SPECIES, e.read_u64(party + 0x58 + 8 * i)) & 0xFFFF for i in range(6)]
    for s in species + [0, t.rng.randrange(1, 900), species[0] | 0x10000]:
        t.check(t.call(HAS_SPECIES, party, s) & 0xFF)


def change(t, party, res, other):
    """One random change; returns False when the party is left unusable (a member dropped)."""
    e = t.emu
    count = e.read(party + 0x88, 1)[0]
    op = t.rng.randrange(13)
    if op <= 2 and e.read(party + 0x89, 1)[0] >= 6:   # Add would look at a missing partner (a fault)
        t.call(UPDATE_PARTNER, party)
    elif op <= 2:
        t.check(t.call(ADD, party, pokemon(t, res)) & 0xFF)
    elif op == 3:
        index = t.rng.choice([0, 1, 2, 3, 4, 5, count, count, 6, 7, 0xFFFFFFFF])
        t.call(SET, party, index | HIGH if t.rng.random() < 0.2 else index, pokemon(t, res))
    elif op <= 5:
        index = t.rng.choice([0, 1, 2, 3, 4, 5, max(count - 1, 0), 6, 0xFFFFFFFF])
        t.call(REMOVE, party, index | HIGH if t.rng.random() < 0.2 else index)
    elif op == 6:
        a, b = (t.rng.choice([0, 1, 2, 3, 4, 5, 5, 6, 0x105]) for _ in range(2))
        t.call(SWAP, party, a | t.rng.choice([0, HIGH]), b)
    elif op == 7:
        order = list(range(count))
        t.rng.shuffle(order)
        dropped = count > 1 and t.rng.random() < 0.1   # a repeat: a member is let go of
        if dropped:
            i, j = t.rng.sample(range(count), 2)
            order[i] = order[j]
        t.call(REORDER, party, e.alloc(bytes(order + [0xEE] * (6 - count))))
        return not dropped
    elif op == 8:
        t.call(SET_PARTNER, party, t.rng.choice([0, 1, 2, 5, 6, 0xFF, t.rng.randrange(256)]) | t.rng.choice([0, 0x300]))
        if t.rng.random() < 0.5:
            t.call(UPDATE_PARTNER, party)
    elif op == 9:
        t.call(HEAL_ALL, party)
    elif op == 10:
        t.call(COMPACT, party)
    elif op == 11:
        t.call(COPY_FROM, party, other)
    else:
        t.call(CLEAR, party)
    return True


def test_party_construction(t):
    res = pkm.FakeResource(t.emu)
    for _ in range(4):
        party = make_party(t, res)
        t.check(snapshot(t, party, res))


def test_party_create(t):
    """This file's copy of gfl::Create<PartyPokemon>, called as the constructor does."""
    res = pkm.FakeResource(t.emu)
    for _ in range(4):
        to.SYS[0].randomize(t.rng)
        to.set_current_resource(t, res)
        species, level = t.rng.choice([0, 25, 151]), t.rng.choice([1, 5, 100, 0x10005])
        refs = t.emu.alloc(struct.pack("<Qiii", res.addr, species, level, t.rng.choice([0, 12345, -1])))
        obj = t.call(CREATE, t.emu.alloc(8), res.addr, refs, refs + 8, refs + 12, refs + 16)
        t.check((obj, member_state(t, obj), res.state()))


def test_party_destruction(t):
    res = pkm.FakeResource(t.emu)
    for dtor, thunk in DTORS * 2:
        party = make_party(t, res)
        fill(t, party, res)
        for i in range(6):                         # a member held elsewhere outlives the party
            if t.rng.random() < 0.3:
                t.call(ADD_REF, t.emu.read_u64(party + 0x58 + 8 * i))
        t.call(dtor, party + 0x48 if thunk else party)
        t.check(snapshot(t, party, res))


def test_party_changes(t):
    res = pkm.FakeResource(t.emu)
    for _ in range(6):
        party, other = make_party(t, res), make_party(t, res)
        fill(t, other, res)
        fill(t, party, res)
        for _ in range(30):
            usable = change(t, party, res, other)
            t.check(snapshot(t, party, res))
            if not usable:
                break
            queries(t, party)


def test_full_party(t):
    """Adding to a full party, and changes at the edges of one."""
    res = pkm.FakeResource(t.emu)
    for _ in range(3):
        party = make_party(t, res)
        fill(t, party, res, 9)
        queries(t, party)
        for index in (5, 0, 5, 3):
            t.call(REMOVE, party, index)
            t.check(snapshot(t, party, res))
            t.call(SET, party, 5, pokemon(t, res))
            t.check(snapshot(t, party, res))
            queries(t, party)


def test_party_save_and_load(t):
    res = pkm.FakeResource(t.emu)
    e = t.emu
    for _ in range(6):
        party = make_party(t, res)
        fill(t, party, res)
        if t.rng.random() < 0.5:
            t.call(SET_PARTNER, party, t.rng.randrange(7))
        outs = [e.alloc(b"\xcc" * SAVED_SIZE) for _ in range(4)]
        t.check(t.call(SERIALIZE_CORES, party, outs[0]) & 0xFFFFFFFF)
        t.check(t.call(CORES_SIZE, party) & 0xFFFFFFFF)
        t.check(t.call(SERIALIZE_WITH, party, outs[1], 1) & 0xFFFFFFFF)
        t.check(t.call(SERIALIZE_WITH, party, outs[2], 0) & 0xFFFFFFFF)
        t.check(t.call(SERIALIZE, party, outs[3]) & 0xFFFFFFFF)
        t.check([e.read(out, SAVED_SIZE) for out in outs])
        t.check(snapshot(t, party, res))
        saved = e.read(outs[1], SAVED_SIZE)
        for _ in range(4):                         # load what was saved, or that changed
            data = bytearray(saved)
            for i in range(6):
                if t.rng.random() < 0.3:
                    rec = pkm.random_record(t.rng, bad_egg=t.rng.random() < 0.2)
                    data[i * RECORD_SIZE:(i + 1) * RECORD_SIZE] = rec[2] + rec[3]
            if t.rng.random() < 0.5:
                data[0x618] = t.rng.choice([0, 1, 3, 6, 7, 0xFF])
            if t.rng.random() < 0.5:
                data[0x619] = t.rng.randrange(256)
            target = make_party(t, res) if t.rng.random() < 0.3 else party
            how = t.rng.randrange(4)
            if how == 0:
                t.call(DESERIALIZE_CORES, target, e.alloc(bytes(data)))
            elif how == 1:
                t.call(DESERIALIZE, target, e.alloc(bytes(data)))
            else:
                t.call(DESERIALIZE_WITH, target, e.alloc(bytes(data)), how - 2)
            t.check(snapshot(t, target, res))
            queries(t, target)
