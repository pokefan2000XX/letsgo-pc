"""decomp/src/battle/condition_turn.cpp's conditions' turns against the originals: Bind, Leech Seed, Perish Song,
Ingrain and Aqua Ring on made-up Pokémon (their conditions' 8 bytes, turn counters, HP and maximum HP made
up), with the records they queue, the Pokémon at the seeder's position and a Pokémon's position faked."""
import struct

TURNS = {"PerishSongTurn": 0x7100A14140, "LeechSeedTurn": 0x7100A14210, "IngrainTurn": 0x7100A143A0,
         "BindTurn": 0x7100A14450, "AquaRingTurn": 0x7100A14620}
# faked: the rest of the battle
GET_ID, GET_VALUE, NEW_RECORD, PUSH_RECORD, AT_POSITION, POSITION_OF = (
    0x71009E2F30, 0x71009E3690, 0x7100A0AFC0, 0x7100A0B020, 0x7100A09650, 0x71009FA740)
CONDITIONS = (8, 0x12, 0x14, 0x15, 0x24)
# the binding moves with an effect of their own, others, and values with bits above a move's
MOVES = (20, 35, 83, 128, 250, 328, 463, 611, 0, 1, 21, 129, 251, 610, 612, 0x10014, 0xFFFF, 0x3FFFF)
U8, U32 = 0xFF, 0xFFFFFFFF
RECORDS = 3                   # Leech Seed queues three
STATE = {}                    # the made-up battle, set by each case


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def pokemon(e):
        return STATE["pokemon"] if e.reg(0) == STATE["mon"] else {}

    def get_id(e):
        record("GetId", e.reg(0))
        return pokemon(e).get("id", 0x1F)

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return pokemon(e).get(e.reg(1) & U32, 0)

    def new_record(e):
        record("NewRecord", e.reg(0), e.reg(1) & U32, e.reg(2) & U8)
        return STATE["free"].pop(0) if STATE["free"] else 0

    def push_record(e):
        record("PushRecord", e.reg(0), e.reg(1), e.read(e.reg(1), 0x40))

    def at_position(e):
        record("PokemonAtPosition", e.reg(0), e.reg(1) & U8)
        return STATE["at"]

    def position_of(e):
        record("PositionOf", e.reg(0), e.reg(1) & U8)
        return STATE["position"]
    emu.stub_function(GET_ID, "GetId", get_id)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(NEW_RECORD, "NewRecord", new_record)
    emu.stub_function(PUSH_RECORD, "PushRecord", push_record)
    emu.stub_function(AT_POSITION, "PokemonAtPosition", at_position)
    emu.stub_function(POSITION_OF, "PositionOf", position_of)


def entry(r, value):
    """A condition's 8 bytes: a kind (0 off, 1 to 3 the value from bit 14, 4 from bit 20), the cause in bits 3-7,
    turns in bits 8-13 and the flag bits 30 and 36 at random."""
    kind = r.choice([0, 1, 2, 3, 4, 4, 2, 5, 7])
    bits = r.getrandbits(64) & ~7 | kind
    if kind in (1, 2, 3):
        bits = bits & ((1 << 14) - 1) | value << 14 | r.getrandbits(1) << 30
    elif kind == 4:
        bits = bits & ((1 << 20) - 1) | value << 20 | r.getrandbits(1) << 36
    return bits & (1 << 64) - 1


class Case:
    """A made-up Pokémon and battle for one call: the objects are made once per emulator and filled anew."""

    def __init__(self, t):
        e, r = t.emu, t.rng
        if not hasattr(e, "condturn"):
            e.condturn = [e.alloc(0x100), e.alloc(0x400)] + [e.alloc(0x40) for _ in range(RECORDS)]
        self.flow, self.mon = e.condturn[:2]
        self.records = e.condturn[2:]
        data = bytearray(r.getrandbits(8) for _ in range(0x400))
        for c in CONDITIONS:
            value = r.choice(MOVES) if c == 8 else r.choice([0, 1, 2, 3, 4, 5, 0x1F, 0x101, r.getrandbits(16)])
            struct.pack_into("<Q", data, 0x98 + 8 * c, entry(r, value))
            data[0x1E8 + c] = r.choice([0, 1, 2, 3, 4, 7, 63, 64, 0xFF])
        e.write(self.mon, bytes(data))
        for rec in self.records:
            e.write(rec, bytes(r.getrandbits(8) for _ in range(0x40)))
        most = r.choice([1, 2, 7, 8, 15, 16, 17, 48, 100, 255, 0xFFFF, 0x10000, 0x10007, r.getrandbits(32)])
        hp = r.choice([0, 0, most, most, 1, 7, 8, 15, 16, 100, 0xFFFF, 0x10000, r.getrandbits(32)])
        STATE.clear()
        STATE.update(t=t, mon=self.mon, free=list(self.records),
                     pokemon={"id": r.choice([0, 1, 3, 5, 0x1E, 0x101]), 0xD: hp, 0xE: most},
                     at=r.choice([0x1F, 0x1F, 0, 1, 3, 5, 0x1E, 0x11F, 0x101]),
                     position=r.choice([0, 1, 2, 3, 4, 0x104]))

    def after(self, t):
        for rec in self.records:
            t.check(t.emu.read(rec, 0x40))


def run(t, name, cases):
    for _ in range(cases):
        case = Case(t)
        t.call(TURNS[name], case.flow, case.mon)
        case.after(t)


def test_bind_turn(t):
    run(t, "BindTurn", 2500)


def test_leech_seed_turn(t):
    run(t, "LeechSeedTurn", 2500)


def test_perish_song_turn(t):
    run(t, "PerishSongTurn", 2000)


def test_heal_turns(t):
    run(t, "IngrainTurn", 1500)
    run(t, "AquaRingTurn", 1500)
