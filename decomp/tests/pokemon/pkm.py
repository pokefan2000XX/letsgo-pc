"""Shared helpers for the Pokémon tests: records, accessors and fakes for what they call."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from fakes import FakeResource, FakeSystem, GET_SYSTEM  # noqa: E402,F401

CORE_SIZE, CALC_SIZE = 0xE8, 0x1C
DEFAULTS = 0x710163EEC4                  # pokemon::g_badEggCalc .. g_discardBlockD (0x1F8 bytes)
DEFAULTS_SIZE = 0x710163F0BC - DEFAULTS
INIT_DEFAULT_RECORDS = 0x7100738770

PERSONAL_LOAD, PERSONAL_GET_PARAM = 0x710071FA80, 0x710071FAA0
LOAD_GROWTH_TABLE, GET_MIN_EXP = 0x710071FAE0, 0x710071FB70
CAN_LEARN_MACHINE, LOAD_LEVEL_UP_MOVES, LEVEL_UP_MOVE_COUNT = 0x710071FAC0, 0x710071FBA0, 0x710071FC30
LEVEL_UP_MOVE_LEVEL, LEVEL_UP_MOVE, LEVEL_UP_MOVE_KIND = 0x710071FC40, 0x710071FC60, 0x710071FC80
GET_MAX_PP, GET_MOVE_TYPE = 0x710073C840, 0x710073C950
LOAD_EVOLUTIONS, EVOLUTION_COUNT, EVOLUTION_METHOD = 0x710071FCA0, 0x710071FD10, 0x710071FD20
EVOLUTION_PARAM, EVOLUTION_SPECIES, EVOLUTION_FORM = 0x710071FD40, 0x710071FD60, 0x710071FD80
HAS_EVOLUTION_FORM, EVOLUTION_LEVEL = 0x710071FDA0, 0x710071FDC0
LOAD_MEGA_EVOLUTIONS, MEGA_EVOLUTION_COUNT = 0x710071FDE0, 0x710071FE40
MEGA_EVOLUTION_METHOD, MEGA_EVOLUTION_ITEM, MEGA_EVOLUTION_FORM = 0x710071FE50, 0x710071FE70, 0x710071FE90
OPERATOR_DELETE = 0x7100003F40
GET_TLS_VALUE = "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE"


def crypt(data, seed):
    """The record encryption (its own inverse)."""
    out = bytearray(data)
    for i in range(0, len(out) - 1, 2):
        seed = (seed * 0x41C64E6D + 0x6073) & 0xFFFFFFFF
        struct.pack_into("<H", out, i, struct.unpack_from("<H", out, i)[0] ^ (seed >> 16))
    return bytes(out)


def checksum(blocks):
    return sum(struct.unpack("<%dH" % (len(blocks) // 2), blocks)) & 0xFFFF


def random_record(rng, bad_egg=False, flags=None):
    """A random record: (decrypted core, decrypted calc, encrypted core, encrypted calc)."""
    key = rng.getrandbits(32)
    blocks = bytes(rng.randrange(256) for _ in range(0xE0))
    flags = rng.choice([0, 0, 0, 1, 2, 0x8000]) if flags is None else flags
    total = checksum(blocks) ^ (rng.randrange(1, 0x10000) if bad_egg else 0)
    head = struct.pack("<IHH", key, flags, total)
    calc = bytes(rng.randrange(256) for _ in range(CALC_SIZE))
    return head + blocks, calc, head + crypt(blocks, key), crypt(calc, key)


class Record:
    """An accessor (0x20 bytes) over a record in guest memory, in a chosen state."""

    def __init__(self, t, state="encrypted", calc=True, bad_egg=False):
        e = t.emu
        core, calc_plain, core_enc, calc_enc = random_record(t.rng, bad_egg)
        plain = state != "encrypted"
        self.core = e.alloc(core if plain else core_enc)
        self.calc = e.alloc(calc_plain if plain else calc_enc) if calc else 0
        self.obj = e.alloc(0x20)
        encrypted, fast = {"encrypted": (1, 0), "fast": (0, 1), "decrypted": (0, 0)}[state]
        e.write(self.obj, struct.pack("<QQQBB", 0x7101234560, self.calc, self.core, encrypted, fast))
        self.t = t

    def snapshot(self):
        """Everything an accessor may change: its state, the record and the default records."""
        e = self.t.emu
        out = e.read(self.obj + 8, 0x12) + e.read(self.core, CORE_SIZE)
        if self.calc:
            out += e.read(self.calc, CALC_SIZE)
        return out + e.read(DEFAULTS, DEFAULTS_SIZE)


STATES = [("encrypted", True, False), ("encrypted", False, False), ("fast", True, False), ("decrypted", True, False),
          ("encrypted", True, True), ("fast", False, True), ("decrypted", False, True)]


def fake_personal(emu):
    """Species data: deterministic values per species, form and parameter."""
    cur = {"species": 0, "form": 0, "growth": 0}
    ratios = [0, 0xFE, 0xFF, 0x1F, 0x7F, 0xBF]

    def load(e):
        cur["species"], cur["form"] = e.reg(0) & 0xFFFFFFFF, e.reg(1) & 0xFFFFFFFF

    def get_param(e):
        p, sp, fm = e.reg(0) & 0xFFFFFFFF, cur["species"], cur["form"]
        if p == 0x14:
            return ratios[sp % len(ratios)]
        return (sp * 7 + fm * 13 + p * 31) & 0xFFFF

    def load_growth(e):
        cur["growth"] = (e.reg(0) + e.reg(1)) % 3

    def min_exp(e):
        lv = min(e.reg(0) & 0xFF, 100)
        return [lv ** 3, lv ** 3 * 4 // 5, lv ** 3 * 5 // 4][cur["growth"]]

    def can_learn_machine(e):
        return (cur["species"] * 3 + cur["form"] + (e.reg(0) & 0xFFFF)) % 5 == 0

    def load_level_up_moves(e):
        cur["moves"] = level_up_moves(e.reg(0) & 0xFFFFFFFF, e.reg(1) & 0xFFFFFFFF)

    def entry(e):               # an index past the end reads the first entry
        moves, index = cur["moves"], e.reg(0) & 0xFFFF
        return moves[index if index < len(moves) else 0] if moves else (0, 0)

    def move_kind(e):
        if (e.reg(0) & 0xFFFF) >= len(cur["moves"]):
            return 2
        return min(entry(e)[0], 2)

    emu.stub_function(PERSONAL_LOAD, "personal::Load", load)
    emu.stub_function(PERSONAL_GET_PARAM, "personal::GetParam", get_param)
    emu.stub_function(LOAD_GROWTH_TABLE, "personal::LoadGrowthTable", load_growth)
    emu.stub_function(GET_MIN_EXP, "personal::GetMinExp", min_exp)
    emu.stub_function(CAN_LEARN_MACHINE, "personal::CanLearnMachine", can_learn_machine)
    emu.stub_function(LOAD_LEVEL_UP_MOVES, "personal::LoadLevelUpMoves", load_level_up_moves)
    emu.stub_function(LEVEL_UP_MOVE_COUNT, "personal::GetLevelUpMoveCount", lambda e: len(cur["moves"]) & 0xFF)
    emu.stub_function(LEVEL_UP_MOVE_LEVEL, "personal::GetLevelUpMoveLevel", lambda e: entry(e)[0])
    emu.stub_function(LEVEL_UP_MOVE, "personal::GetLevelUpMove", lambda e: entry(e)[1])
    emu.stub_function(LEVEL_UP_MOVE_KIND, "personal::GetLevelUpMoveKind", move_kind)
    cur["moves"] = []


def level_up_moves(species, form):
    """A species' level-up moves, (level, move) in level order: a few evolution moves (level 0), then
    moves every 0-2 levels up to 100. Moves come from 1-40, so they repeat."""
    seed = species * 31 + form
    moves = [(0, (seed + i * 11) % 40 + 1) for i in range(seed % 3)]
    level, i = 1, 0
    while level <= 100:
        moves.append((level, (seed * 5 + i * 7) % 40 + 1))
        level += (seed + i) % 3
        i += 1
    return moves


def fake_moves(emu):
    """Move data: a base PP of 1-40 per move, and a type of 0-17."""
    def max_pp(e):
        move, ups = e.reg(0) & 0xFFFFFFFF, e.reg(1) & 0xFF
        base = move * 7 % 40 + 1
        return (base + base * min(ups, 3) * 20 // 100) & 0xFF

    emu.stub_function(GET_MAX_PP, "moves::GetMaxPp", max_pp)
    emu.stub_function(GET_MOVE_TYPE, "moves::GetType", lambda e: (e.reg(0) & 0xFFFFFFFF) % 18)


def evolutions(species, form):
    """A species' evolution list: (method, param, species, form or 0xFF, level). Methods cover
    0-44 (past the last one too); parameters are small, so items, moves, types and versions match."""
    seed = species * 7 + form
    return [((seed + i * 13) % 45, (seed + i * 5) % 12, (species + i + 1) % 900,
             0xFF if (seed + i) % 3 else i + 1, (seed * 3 + i * 5) % 60) for i in range(seed % 4)]


def mega_evolutions(species):
    """A species' Mega Evolutions: (form, method 0-3, item 0-9)."""
    return [(i + 1, (species + i) % 4, (species * 5 + i) % 10) for i in range(species % 3)]


def fake_evolutions(emu):
    """The evolution and Mega Evolution lists (evolutions(), mega_evolutions())."""
    cur = {"evo": [], "mega": []}

    def load(e):
        cur["evo"] = evolutions(e.reg(0) & 0xFFFF, e.reg(1) & 0xFF)

    def load_mega(e):
        cur["mega"] = mega_evolutions(e.reg(0) & 0xFFFF)

    def field(table, column, limit):
        def get(e):
            entries, index = cur[table], e.reg(0) & 0xFF
            if index >= limit or not entries:
                return 0
            return entries[index if index < len(entries) else 0][column]
        return get

    emu.stub_function(LOAD_EVOLUTIONS, "personal::LoadEvolutions", load)
    emu.stub_function(EVOLUTION_COUNT, "personal::GetEvolutionCount", lambda e: len(cur["evo"]))
    for addr, column in ((EVOLUTION_METHOD, 0), (EVOLUTION_PARAM, 1), (EVOLUTION_SPECIES, 2),
                         (EVOLUTION_FORM, 3), (EVOLUTION_LEVEL, 4)):
        emu.stub_function(addr, "personal evolution field %d" % column, field("evo", column, 8))
    has_form = field("evo", 3, 8)
    emu.stub_function(HAS_EVOLUTION_FORM, "personal::HasEvolutionForm", lambda e: has_form(e) != 0xFF)
    emu.stub_function(LOAD_MEGA_EVOLUTIONS, "personal::LoadMegaEvolutions", load_mega)
    emu.stub_function(MEGA_EVOLUTION_COUNT, "personal::GetMegaEvolutionCount", lambda e: len(cur["mega"]))
    for addr, column in ((MEGA_EVOLUTION_FORM, 0), (MEGA_EVOLUTION_METHOD, 1), (MEGA_EVOLUTION_ITEM, 2)):
        emu.stub_function(addr, "personal mega field %d" % column, field("mega", column, 2))


SPEC = struct.Struct("<QQQIBxHHhBB6h2xIBBHH2x")      # pokemon::CreationSpec
assert SPEC.size == 0x40
PID_NOT_SHINY, PID_SHINY, PID_ROLLS, RANDOM = 0x1FFFFFFFF, 0x2FFFFFFFF, 0x3FFFFFFFF, (1 << 64) - 1


def random_spec(rng, species=None):
    """A CreationSpec with each field either left to chance or set, sometimes out of range."""
    def pick(chance, value, otherwise):
        return value if rng.random() < chance else otherwise

    pid = rng.choice([PID_NOT_SHINY, PID_SHINY, PID_ROLLS, PID_ROLLS, rng.getrandbits(32), rng.getrandbits(64)])
    size = lambda: rng.choice([0x100, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x7FFF, 0x8000, 0xFFFF,
                               rng.randrange(256), rng.getrandbits(16)])
    ivs = [pick(0.5, -1, rng.choice([0, 31, rng.randrange(32), rng.randrange(-0x8000, 0x8000)])) for _ in range(6)]
    return SPEC.pack(
        pick(0.5, RANDOM, rng.getrandbits(rng.choice([32, 64]))), pid, pick(0.5, RANDOM, rng.getrandbits(32)),
        rng.randrange(1, 900) if species is None else species, pick(0.3, 0xFF, rng.randrange(256)),
        rng.choice([1, 5, 50, 100, 101, rng.getrandbits(16)]), pick(0.5, 0xFF, rng.randrange(4)),
        pick(0.5, -1, rng.randrange(-30, 40)), rng.choice([0, 1, 2, 3, 0xFE, 0xFF, 0xFF]),
        rng.choice([0, 1, 1, 3, 16, 255]), *ivs, pick(0.5, 0xFFFF, rng.choice([0, 70, 255, rng.getrandbits(32)])),
        rng.choice([0, 0, 1, 3, 5, 6, 7, 255]), rng.choice([0, 1, 1, 2, 5]), size(), size())
