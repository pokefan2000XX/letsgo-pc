"""Shared helpers for the Pokémon tests: records, accessors and fakes for what they call."""
import struct

CORE_SIZE, CALC_SIZE = 0xE8, 0x1C
DEFAULTS = 0x710163EEC4                  # pokemon::g_badEggCalc .. g_discardBlockD (0x1F8 bytes)
DEFAULTS_SIZE = 0x710163F0BC - DEFAULTS
INIT_DEFAULT_RECORDS = 0x7100738770

PERSONAL_LOAD, PERSONAL_GET_PARAM = 0x710071FA80, 0x710071FAA0
LOAD_GROWTH_TABLE, GET_MIN_EXP = 0x710071FAE0, 0x710071FB70
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

    emu.stub_function(PERSONAL_LOAD, "personal::Load", load)
    emu.stub_function(PERSONAL_GET_PARAM, "personal::GetParam", get_param)
    emu.stub_function(LOAD_GROWTH_TABLE, "personal::LoadGrowthTable", load_growth)
    emu.stub_function(GET_MIN_EXP, "personal::GetMinExp", min_exp)


class FakeResource:
    """A gfl::mem::MemoryResource whose allocations come from the emulator's heap (filled with 0xCC
    so missing initialization shows), and the thread's resource stack it gets pushed on."""

    def __init__(self, emu):
        self.log = []

        def allocate(e):
            size, align = e.reg(1), e.reg(2)
            self.log.append(("allocate", size, align))
            return e.alloc(b"\xcc" * max(size, 1), align=max(align, 16))

        def deallocate(e):
            self.log.append(("deallocate", e.reg(2), e.reg(3)))

        slots = [emu.host_function("resource dtor", lambda e: None), emu.host_function("resource dtor", lambda e: None),
                 emu.host_function("resource allocate", allocate), emu.host_function("resource deallocate", deallocate),
                 emu.host_function("resource is_equal", lambda e: 0)]
        vtable = emu.alloc(struct.pack("<5Q", *slots))
        self.addr = emu.alloc(struct.pack("<QQ", vtable, 0))
        self.stack = emu.alloc(0x88)
        emu.import_handlers[GET_TLS_VALUE] = lambda e: self.stack
