"""decomp/src/battle/side_out.cpp and battle_pokemon.cpp's CanFight against the originals: whether a side has no
Pokémon left that can fight, on made-up battles (which clients take part, their parties, each Pokémon's HP and
whether it's an egg made up), and the setup's sides and clients, a party's count and whether a Pokémon can fight
on their own."""
import struct

IS_SIDE_OUT, ENDS_WHEN_SIDE_OUT, COUNT_FIGHTERS, CAN_FIGHT = 0x71009F2D80, 0x7100A54DD0, 0x71009DF260, 0x71009E3E60
SIDE_OF_CLIENT, HAS_SIDE, SIDE_OF_CLIENT_IN, HAS_SIDE_IN = 0x71009D7120, 0x71009D9B80, 0x71009E1FA0, 0x71009E1EF0
# faked: the rest of the battle and the Pokémon objects
HAS_CLIENT, GET_VALUE, IS_EGG = 0x71009D3DD0, 0x71009E3690, 0x7100727FB0
U8, U32 = 0xFF, 0xFFFFFFFF
STATE = {}                    # the made-up battle, set by each case


def setup(emu):
    def record(name, *args):
        STATE["t"].check((name,) + args)

    def has_client(e):
        record("HasClient", e.reg(0), e.reg(1) & U8)
        return STATE["clients"].get(e.reg(1) & U8, 0)

    def get_value(e):
        record("GetValue", e.reg(0), e.reg(1) & U32)
        return STATE["hp"].get(e.reg(0), 0) if (e.reg(1) & U32) == 0xD else 0

    def is_egg(e):
        record("IsEgg", e.reg(0), e.reg(1) & U32)
        return STATE["egg"].get(e.reg(0), 0)
    emu.stub_function(HAS_CLIENT, "HasClient", has_client)
    emu.stub_function(GET_VALUE, "GetValue", get_value)
    emu.stub_function(IS_EGG, "IsEgg", is_egg)


class Battle:
    """A flow with a setup and a list of four parties of made-up Pokémon (each a battle Pokémon pointing at a
    Pokémon object at +0x60), made once per emulator and filled anew for each case."""

    def __init__(self, t):
        e, r = t.emu, t.rng
        if not hasattr(e, "sideout"):
            e.sideout = {"flow": e.alloc(0x100), "setup": e.alloc(0x200), "list": e.alloc(0x200),
                         "mons": [e.alloc(0x100) for _ in range(24)], "objects": [e.alloc(0x40) for _ in range(24)]}
        s = e.sideout
        self.flow, self.setup, self.list = s["flow"], s["setup"], s["list"]
        e.write(self.flow, bytes(r.getrandbits(8) for _ in range(0x100)))
        e.write(self.flow + 0x60, struct.pack("<QQ", self.setup, self.list))
        e.write(self.setup, bytes(r.getrandbits(8) for _ in range(0x200)))
        e.write(self.list, bytes(r.getrandbits(8) for _ in range(0x200)))
        STATE.clear()
        STATE["t"] = t
        STATE["hp"], STATE["egg"] = {}, {}
        STATE["clients"] = {c: r.choice([0, 1, 1, 0x100, 3]) for c in range(4)}
        self.parties = []
        for client in range(4):
            count = r.choice([0, 1, 1, 2, 3, 6, 6])
            members = []
            for slot in range(6):
                k = client * 6 + slot
                mon, obj = s["mons"][k], s["objects"][k]
                e.write(mon + 0x60, struct.pack("<Q", obj))
                STATE["hp"][mon] = r.choice([0, 0, 0, 1, 20, 0x10000, 0x80000000, r.getrandbits(32)])
                STATE["egg"][obj] = r.choice([0, 0, 0, 1, 0x100])
                members.append(mon)
            party = self.list + 0x60 + client * 0x38
            e.write(party, struct.pack("<6QB", *members, count))
            self.parties.append(party)


def test_side_out(t):
    for _ in range(3000):
        b = Battle(t)
        t.check(t.call(IS_SIDE_OUT, b.flow) & U8)


def test_party_and_pokemon(t):
    for _ in range(600):
        b = Battle(t)
        for party in b.parties:
            t.check(t.call(COUNT_FIGHTERS, party) & U32)
        for mon in t.emu.sideout["mons"][:6]:
            t.check(t.call(CAN_FIGHT, mon) & U8)


def test_sides_and_clients(t):
    r = t.rng
    b = Battle(t)
    values = list(range(0, 300)) + [0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFC, 0x100000003] + \
        [r.getrandbits(64) for _ in range(200)]
    for v in values:
        t.emu.write(b.setup + 0x168, struct.pack("<I", r.getrandbits(32)))
        t.check(t.call(SIDE_OF_CLIENT, b.setup, v) & U8)
        t.check(t.call(HAS_SIDE, b.setup, v) & U8)
        t.check(t.call(SIDE_OF_CLIENT_IN, r.getrandbits(32), v) & U8)
        t.check(t.call(HAS_SIDE_IN, r.getrandbits(32), v) & U8)
        t.check(t.call(ENDS_WHEN_SIDE_OUT, v) & U8)
