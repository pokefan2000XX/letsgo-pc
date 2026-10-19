"""decomp/src/pokemon/personal_table.cpp, learnset.cpp and personal.cpp against the originals: species
entries, level-up moves and their cache, the lookups (over logged fakes of the evolution, Mega
Evolution and EXP tables), species names, and making and destroying all the tables, from fake files."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fakes  # noqa: E402

U8, U16, U32 = 0xFF, 0xFFFF, 0xFFFFFFFF
# species entries
PT_LOAD_ALL, PT_UNLOAD_ALL, PT_CTOR, PT_DTOR, PT_DELETE = 0x710071D800, 0x710071DAD0, 0x710071DB30, 0x710071DC60, 0x710071DCA0
PT_SPECIES, PT_FORM, PT_LOAD, PT_ENTRY = 0x710071DCF0, 0x710071DD00, 0x710071DD10, 0x710071DDD0
PT_PARAM, PT_MACHINE = 0x710071DE40, 0x710071E0E0
# learnsets
LS_SET_SOURCE, LS_CLEAR_SOURCE, LS_CTOR, LS_DTOR = 0x7100720240, 0x7100720260, 0x7100720270, 0x7100720390
LS_SPECIES, LS_FORM, LS_LOAD, LS_START, LS_FINISH = 0x71007203C0, 0x71007203D0, 0x71007203E0, 0x7100720600, 0x71007206C0
LS_COPY, LS_COUNT, LS_KIND, LS_LEVEL, LS_MOVE = 0x7100720750, 0x71007207A0, 0x71007207B0, 0x71007207F0, 0x7100720810
CACHE_CTOR, CACHE_DTOR, CACHE_DELETE, CACHE_FIND, CACHE_ADD = 0x7100720830, 0x7100720AB0, 0x7100720B60, 0x7100720C00, 0x7100720CB0
# the system
INITIALIZE, FINALIZE = 0x710071E8D0, 0x710071F540
NAME_U16, NAME_STRBUF = 0x710071F980, 0x710071FA10
WRAP_LOAD, WRAP_PARAM, WRAP_MACHINE, WRAP_GROWTH, WRAP_MIN_EXP = 0x710071FA80, 0x710071FAA0, 0x710071FAC0, 0x710071FAE0, 0x710071FB70
WRAP_MOVES, WRAP_MOVE_COUNT, WRAP_MOVE_LEVEL, WRAP_MOVE, WRAP_MOVE_KIND = 0x710071FBA0, 0x710071FC30, 0x710071FC40, 0x710071FC60, 0x710071FC80
WRAP_EVOS = 0x710071FCA0
EVO_GETTERS = [(0x710071FD20, U16), (0x710071FD40, U16), (0x710071FD60, U16), (0x710071FD80, U8), (0x710071FDA0, U8),
               (0x710071FDC0, U8)]
WRAP_EVO_COUNT, WRAP_MEGAS, WRAP_MEGA_COUNT = 0x710071FD10, 0x710071FDE0, 0x710071FE40
MEGA_GETTERS = [(0x710071FE50, U16), (0x710071FE70, U16), (0x710071FE90, U8)]
HAS_MEGA_FORM, IS_PRIMAL, IS_METEOR, METEOR_FORM, CORE_FORM = 0x710071FEB0, 0x710071FF80, 0x710071FFC0, 0x710071FFD0, 0x7100720000
OPERATOR_DELETE = 0x7100003F40
# globals: (address, what) for the snapshot
PERSONAL_SOURCE, PERSONAL_DATA, NAME_BUFFER, NAMES, LEARNSET_SOURCE = 0x710163EA68, 0x710163EA70, 0x710163EA90, 0x710163EC78, 0x710163ECD0
OWNERS = {name: 0x710163EA98 + 0x20 * i for i, name in enumerate(
    ["unk cache", "personal", "growth personal", "growth", "growth cache", "level-up moves", "no species moves",
     "unk learnset", "learnset cache", "evolutions", "evolution cache", "mega check", "mega evolutions", "mega cache",
     "pms"])}
# outside D1
PERSONAL_PATH, LEARNSET_PATH, PMS_PATH = 0x710073B330, 0x710073B490, 0x710073B7D0
GET_FILE_LOADER, LOADER_READ, LOADER_IS_DONE = 0x710073AB70, 0x710073A230, 0x710073A9C0
MSG_CTOR, MSG_GET_STRING = 0x7100677B90, 0x71006782C0
SOURCES = [(0x710073E5B0, "set growth source"), (0x710073D830, "set evolution source"), (0x710073ED90, "set mega source"),
           (0x710073E5D0, "clear growth source"), (0x710073D850, "clear evolution source"), (0x710073EDB0, "clear mega source")]
# (constructor, name, takes a resource, takes a capacity)
TABLE_CTORS = [(0x710073D870, "EvolutionTable", True, False), (0x710073E030, "EvolutionCache", True, True),
               (0x710073E5E0, "GrowthTable", False, False), (0x710073E850, "GrowthCache", True, True),
               (0x710073EDC0, "MegaTable", True, False), (0x710073F240, "MegaCache", True, True),
               (0x710073F770, "UnkCache", True, True)]
ENTRIES = 960                                                       # 900 species, then forms


def personal_data(rng):
    """The entries: random, with some species' forms after the 900 species."""
    out = bytearray(rng.randrange(256) for _ in range(ENTRIES * 0x54))
    next_form = 900
    for species in range(900):
        entry = species * 0x54
        count = rng.choice([0, 0, 0, 0, 2, 3, 5])
        if count and next_form + count < ENTRIES:
            struct.pack_into("<H", out, entry + 0x1C, next_form)
            out[entry + 0x20] = count
            next_form += count - 1
        else:
            struct.pack_into("<H", out, entry + 0x1C, 0)
    return bytes(out)


def learnset_file(rng):
    """A learnset: moves and levels, usually ending with {-1, -1}, sometimes too long or empty."""
    count = rng.choice([0, 1, 5, 20, 54, 55, 60])
    data = b"".join(struct.pack("<HH", rng.randrange(1, 800), rng.choice([0, 1, rng.randrange(101)]))
                    for _ in range(count))
    if rng.random() < 0.8:
        data += struct.pack("<HH", U16, U16)
    if rng.random() < 0.2:
        data += struct.pack("<HH", U16, 5)
    return data


class Fakes:
    """Everything outside D1: files, the background loader, message data and the tables' methods,
    each call logged with the objects' names."""

    def __init__(self, emu):
        self.emu, self.files = emu, fakes.FakeFiles(emu)
        self.log = self.files.log
        self.names = self.files.refs.names
        self.loader = emu.alloc(0x40)
        self.pending = {}                                            # buffer -> file name
        self.done_calls = 0
        files = self.files
        files.path(PERSONAL_PATH, lambda e: "personal_total.bin")
        files.path(LEARNSET_PATH, lambda e: "learnset %d" % (e.reg(0) & U32))
        files.path(PMS_PATH, lambda e: "pms_total.bin")

        def read(e):
            name = files.refs.name_of(e.read_u64(e.reg(1)))
            self.pending[e.reg(2)] = name
            self.log.append(("async read", e.reg(0) == self.loader, name, e.reg(3)))

        def is_done(e):
            self.done_calls += 1
            done = self.done_calls % 3 != 0
            buffer = e.reg(1)
            if done and buffer in self.pending:
                data = files.files.get(self.pending.pop(buffer))
                if data:
                    e.write(buffer, data[:0xDC])
            self.log.append(("async done", done))
            return int(done)

        emu.stub_function(GET_FILE_LOADER, "GetFileLoader", lambda e: self.loader)
        emu.stub_function(LOADER_READ, "FileLoader::Read", read)
        emu.stub_function(LOADER_IS_DONE, "FileLoader::IsDone", is_done)
        emu.stub_function(OPERATOR_DELETE, "operator delete", lambda e: self.log.append(("delete", self.name(e.reg(0)))))
        for addr, name in SOURCES:
            emu.stub_function(addr, name, self.logger(name, regs=(0, 1)))
        # the tables' constructors give their objects a vtable whose destructor is logged
        dtor = emu.host_function("table dtor", lambda e: self.log.append(("table dtor", self.name(e.reg(0)))))
        self.table_vtable = emu.alloc(struct.pack("<2Q", dtor, dtor))
        for addr, name, resource, capacity in TABLE_CTORS:
            emu.stub_function(addr, name, self.table_ctor(name, resource, capacity))

        def msg_ctor(e):
            obj = e.reg(0)
            e.write(obj + 0x48, struct.pack("<Q", files.refs.vtable))
            self.names[obj] = "MsgData %s" % files.refs.name_of(e.read_u64(e.reg(1)))
            self.log.append(("MsgData", self.names[obj], e.reg(2) & U32))

        def get_string(e):
            msg, index, out = e.reg(0), e.reg(1) & U32, e.reg(2)
            text = e.read_u64(out + 0x58)
            name = [0x41 + index % 26, 0x61 + index // 26 % 26, 0x30 + len(self.name(msg)) % 10] * (index % 5) + [0]
            e.write(text, struct.pack("<%dH" % len(name), *name))
            self.log.append(("GetString", self.name(msg), index))
            return int(index < 700)

        emu.stub_function(MSG_CTOR, "MsgData::MsgData", msg_ctor)
        emu.stub_function(MSG_GET_STRING, "MsgData::GetString", get_string)

    def name(self, obj):
        return self.names.get(obj, "0x%x" % obj if obj < 0x100000 else "unnamed")

    def logger(self, what, regs=(), result=None):
        def handler(e):
            self.log.append((what,) + tuple(self.name(e.reg(r)) if r == 0 else e.reg(r) & U32 for r in regs))
            return result(e) if result else None
        return handler

    def table_ctor(self, name, resource, capacity):
        def handler(e):
            obj = e.reg(0)
            self.emu.write(obj, struct.pack("<Q", self.table_vtable) + b"\xab" * 0x28)
            self.names[obj] = name + " %d" % sum(1 for n in self.names.values() if n.startswith(name))
            args = (e.reg(1) if resource else None, e.reg(2 if resource else 1) & U32 if capacity else None)
            self.log.append(("ctor", self.names[obj], args))
        return handler


def setup(emu):
    system = fakes.FakeSystem(emu)
    SYSTEM[0] = system
    FAKES[0] = Fakes(emu)


SYSTEM, FAKES = [None], [None]


def set_owner(t, addr, ptr, res, size=0x30, align=8):
    t.emu.write(addr, struct.pack("<4Q", ptr, res.addr, size, align))


def give_data(t, res):
    """g_personalData holds random entries (as if LoadAll had read them)."""
    data = personal_data(t.rng)
    set_owner(t, PERSONAL_DATA, t.emu.alloc(data, align=4), res, len(data), 4)
    t.emu.write(PERSONAL_SOURCE, struct.pack("<Q", 0x7101111100))


def test_personal_entries(t):
    res, f = fakes.FakeResource(t.emu), FAKES[0]
    give_data(t, res)
    obj = t.emu.alloc(b"\xcc" * 0x30)
    t.call(PT_CTOR, obj, res.addr)
    t.check(t.emu.read(obj + 8, 0x28))
    for _ in range(150):
        species = t.rng.choice([0, 1, 25, 151, 809, 810, 811, 900, t.rng.randrange(810)])
        form = t.rng.choice([0, 0, 1, 2, 4, 0x100, t.rng.randrange(256)])
        t.call(PT_LOAD, obj, species, form)
        info = t.emu.read_u64(obj + 0x10)
        t.check((t.emu.read(obj + 8, 8), t.emu.read(info, 0x54), t.call(PT_SPECIES, obj) & U32, t.call(PT_FORM, obj) & U8,
                 t.call(PT_ENTRY, species, form) & U32))
        t.check([t.call(PT_PARAM, obj, p) & U32 for p in list(range(0x34)) + [0x100]])
        t.check([t.call(PT_MACHINE, obj, m) & U8 for m in range(0, 260, 7)])
    t.call(PT_DTOR, obj)
    t.check(t.emu.read(obj + 8, 0x28))
    t.call(PT_CTOR, obj, res.addr)
    t.call(PT_DELETE, obj)
    t.check((res.state(), f.log))


def test_personal_load_all(t):
    res, f = fakes.FakeResource(t.emu), FAKES[0]
    for size in [None, 0, 0x54, 0x54 * 900, 0x54 * 900 + 7]:
        if size is None:
            f.files.files.pop("personal_total.bin", None)
        else:
            f.files.files["personal_total.bin"] = bytes(t.rng.randrange(256) for _ in range(size))
        for _ in range(2):                                               # the second does nothing
            t.call(PT_LOAD_ALL, res.addr, 0x7101111100 + size if size else 0x7101111100)
            ptr, owner, length, align = struct.unpack("<4Q", t.emu.read(PERSONAL_DATA, 0x20))
            t.check((t.emu.read(PERSONAL_SOURCE, 8), owner == res.addr, length, align,
                     t.emu.read(ptr, min(length + 7, 0x54 * 900 + 7)) if ptr else None))
        t.call(PT_UNLOAD_ALL)
        t.call(PT_UNLOAD_ALL)
        t.check((t.emu.read(PERSONAL_SOURCE, 0x28), res.state()))
    t.check(f.log)


def make_learnset(t, res):
    obj = t.emu.alloc(b"\xcc" * 0x30)
    t.call(LS_CTOR, obj, res.addr)
    return obj


def learnset_state(t, obj):
    moves = t.emu.read_u64(obj + 8)
    return t.emu.read(obj, 0x30)[:8] + t.emu.read(obj + 0x28, 1) + t.emu.read(moves, 0xDC)


def test_learnsets(t):
    res, f = fakes.FakeResource(t.emu), FAKES[0]
    give_data(t, res)
    for entry in range(ENTRIES):
        if t.rng.random() < 0.7:
            f.files.files["learnset %d" % entry] = learnset_file(t.rng)
    sets = [make_learnset(t, res) for _ in range(3)]
    t.check([learnset_state(t, s) for s in sets])
    t.call(LS_SET_SOURCE, res.addr, 0x7101222200)
    t.call(LS_SET_SOURCE, res.addr, 0x7101333300)
    t.check(t.emu.read(LEARNSET_SOURCE, 8))
    for _ in range(120):
        s = t.rng.choice(sets)
        species, form = t.rng.choice([0, 1, 25, 151, 809, 900, t.rng.randrange(810)]), t.rng.choice([0, 0, 1, 2, 3])
        what = t.rng.randrange(4)
        if what == 0:
            t.call(LS_LOAD, s, species, form)
        elif what == 1:
            t.check(t.call(LS_START, s, species, form) & U8)
        elif what == 2:
            t.check(t.call(LS_FINISH, s) & U8)
        else:
            t.call(LS_COPY, s, t.rng.choice(sets))
        t.check((learnset_state(t, s), t.call(LS_SPECIES, s) & U32, t.call(LS_FORM, s) & U8, t.call(LS_COUNT, s) & U8))
        t.check([(t.call(LS_KIND, s, i) & U32, t.call(LS_LEVEL, s, i) & U16, t.call(LS_MOVE, s, i) & U16)
                 for i in list(range(0, 58, 3)) + [0x10000]])
    for s in sets:
        t.call(LS_DTOR, s)
    t.call(LS_CLEAR_SOURCE)
    t.check((t.emu.read(LEARNSET_SOURCE, 8), res.state(), f.log))


def test_learnset_cache(t):
    res, f = fakes.FakeResource(t.emu), FAKES[0]
    give_data(t, res)
    for entry in range(ENTRIES):
        f.files.files["learnset %d" % entry] = learnset_file(t.rng)
    for capacity in (1, 3, 8):
        cache = t.emu.alloc(b"\xcc" * 0x30)
        t.call(CACHE_CTOR, cache, res.addr, capacity)
        t.check(t.emu.read(cache, 0x30)[8:])
        s = make_learnset(t, res)
        out = make_learnset(t, res)
        for _ in range(40):
            species, form = t.rng.randrange(1, 12), t.rng.choice([0, 1])
            found = t.call(CACHE_FIND, cache, species, form, out) & U8
            if not found:
                t.call(LS_LOAD, s, species, form)
                t.call(CACHE_ADD, cache, s)
            t.check((found, learnset_state(t, out), t.emu.read(cache + 0x28, 8)))
        entries = t.emu.read_u64(cache + 8)
        t.check([learnset_state(t, entries + 0x30 * i) for i in range(capacity)])
        t.call(t.rng.choice([CACHE_DTOR, CACHE_DELETE]), cache)
        t.check(t.emu.read(cache + 8, 0x20))
    t.check((res.state(), f.log))


class FakeTables:
    """The tables outside D1 as logged fakes that answer from what they were loaded with."""

    def __init__(self, t, res):
        self.t, f = t, FAKES[0]
        e = t.emu
        self.loaded = {}

        def obj(name):
            o = e.alloc(0x30)
            f.names[o] = name
            return o

        self.objs = {name: obj(name) for name in ["growth", "growth cache", "evolutions", "evolution cache",
                                                  "mega check", "mega evolutions", "mega cache"]}
        for name, o in self.objs.items():
            set_owner(t, OWNERS[name], o, res)

        def find(regs, key):
            def handler(e):
                args = tuple(e.reg(r) & U32 for r in regs)
                hit = sum(args) % 3 == 0
                if hit:
                    self.loaded[f.name(e.reg(regs[-1] + 1))] = args
                f.log.append(("find", f.name(e.reg(0)), args, f.name(e.reg(regs[-1] + 1)), hit))
                return int(hit)
            return handler

        def load(regs):
            def handler(e):
                self.loaded[f.name(e.reg(0))] = tuple(e.reg(r) & U32 for r in regs)
                f.log.append(("load", f.name(e.reg(0)), self.loaded[f.name(e.reg(0))]))
            return handler

        def get(column, mask):
            def handler(e):
                key = self.loaded.get(f.name(e.reg(0)), (0,))
                value = (sum(key) * 7 + (e.reg(1) & 0xFF) * 13 + column * 31) & mask
                f.log.append(("get", f.name(e.reg(0)), column, e.reg(1) & 0xFF))
                return value
            return handler

        def count(e):
            key = self.loaded.get(f.name(e.reg(0)), (0,))
            f.log.append(("count", f.name(e.reg(0))))
            return sum(key) % 4

        def add(e):
            f.log.append(("add", f.name(e.reg(0)), f.name(e.reg(1))))

        stubs = [(0x710073E430, find((1, 2), 0)), (0x710073E4E0, add), (0x710073DA40, load((1, 2))),
                 (0x710073DC50, count), (0x710073F640, find((1,), 0)), (0x710073F6D0, add), (0x710073EF70, load((1,))),
                 (0x710073F160, count), (0x710073EC50, find((1,), 0)), (0x710073ECE0, add), (0x710073E620, load((1,))),
                 (0x710073E830, get(9, U32))]
        stubs += [(addr, get(i, 0xFFFF)) for i, addr in enumerate(
            [0x710073DCE0, 0x710073DD10, 0x710073DDB0, 0x710073DE50, 0x710073DEF0, 0x710073DF90])]
        stubs += [(addr, get(10 + i, 0xFFFF)) for i, addr in enumerate([0x710073F190, 0x710073F1C0, 0x710073F200])]
        for addr, handler in stubs:
            e.stub_function(addr, "table 0x%x" % addr, handler)


def test_lookups(t):
    res, f = fakes.FakeResource(t.emu), FAKES[0]
    give_data(t, res)
    for entry in range(ENTRIES):
        if t.rng.random() < 0.8:
            f.files.files["learnset %d" % entry] = learnset_file(t.rng)
    FakeTables(t, res)
    for name in ("personal", "growth personal"):
        obj = t.emu.alloc(0x30)
        t.call(PT_CTOR, obj, res.addr)
        set_owner(t, OWNERS[name], obj, res)
    for name in ("level-up moves", "no species moves"):
        set_owner(t, OWNERS[name], make_learnset(t, res), res)
    cache = t.emu.alloc(0x30)
    t.call(CACHE_CTOR, cache, res.addr, 4)
    set_owner(t, OWNERS["learnset cache"], cache, res)
    t.call(LS_LOAD, t.emu.read_u64(OWNERS["no species moves"]), 0, 0)
    for _ in range(80):
        species, form = t.rng.choice([0, 0, 1, 2, 3, 382, 383, t.rng.randrange(900)]), t.rng.choice([0, 0, 1, 2, 0x101])
        t.call(WRAP_LOAD, species, form)
        t.check([t.call(WRAP_PARAM, p) & U32 for p in range(0x32)] + [t.call(WRAP_MACHINE, m) & U8 for m in (0, 31, 100, 0x10005)])
        t.call(WRAP_GROWTH, species, form)
        t.check([t.call(WRAP_MIN_EXP, level) & U32 for level in (0, 1, 50, 99, 100, 101, 255)])
        t.call(WRAP_MOVES, species, form)
        t.check([t.call(WRAP_MOVE_COUNT) & U8] + [(t.call(WRAP_MOVE_LEVEL, i) & U16, t.call(WRAP_MOVE, i) & U16,
                                                    t.call(WRAP_MOVE_KIND, i) & U32) for i in (0, 1, 5, 54, 60)])
        t.call(WRAP_EVOS, species, form)
        t.check([t.call(WRAP_EVO_COUNT) & U8] + [t.call(g, i) & m for g, m in EVO_GETTERS for i in (0, 1, 7)])
        t.call(WRAP_MEGAS, species)
        t.check([t.call(WRAP_MEGA_COUNT) & U8] + [t.call(g, i) & m for g, m in MEGA_GETTERS for i in (0, 1)])
        t.check([t.call(HAS_MEGA_FORM, species, fm) & U8 for fm in (0, 1, 2, 0x101)])
    t.check([t.call(IS_PRIMAL, s, fm) & U8 for s in (381, 382, 383, 384, 0x1017E) for fm in (0, 1, 2, 0x101)])
    t.check([(t.call(IS_METEOR, fm) & U8, t.call(METEOR_FORM, fm) & U8, t.call(CORE_FORM, fm) & U8)
             for fm in list(range(20)) + [0xFF, 0x107]])
    t.check((t.emu.read(OWNERS["level-up moves"] - 0x80, 0x400), res.state(), f.log))


def test_species_names(t):
    f = FAKES[0]
    buffer = t.emu.alloc(0x80)
    text = t.emu.alloc(0x200)
    t.emu.write(buffer + 0x58, struct.pack("<Q", text))
    t.emu.write(NAME_BUFFER, struct.pack("<Q", buffer))
    for i in range(11):
        msg = t.emu.alloc(0x60)
        f.names[msg] = "names %d" % i
        t.emu.write(NAMES + 8 * i, struct.pack("<Q", msg))
    res = fakes.FakeResource(t.emu)
    give_data(t, res)
    out_buffer = t.emu.alloc(0x80)
    out_text = t.emu.alloc(0x200)
    t.emu.write(out_buffer + 0x58, struct.pack("<Q", out_text))
    for _ in range(150):
        species = t.rng.choice([0, 1, 25, 700, 809, 810, 811, 812, 900, -5 & U32, t.rng.randrange(812)])
        language = t.rng.choice([0, 1, 2, 9, 10, 11, U32, t.rng.randrange(11)])
        out = t.emu.alloc(b"\xcc" * 0x20)
        t.emu.write(text, b"\x11\x00" * 20 + b"\0\0")
        t.check((t.call(NAME_U16, out, species, language) & U8, t.emu.read(out, 0x20)))
        t.check((t.call(NAME_STRBUF, out_buffer, species, language) & U8, t.emu.read(out_text, 0x20)))
    t.check(f.log)


def test_setup(t):
    res, f, system = fakes.FakeResource(t.emu), FAKES[0], SYSTEM[0]
    e = t.emu
    sources = [0x7101000000 + 0x100 * i for i in range(5)]
    e.write(system.addr, struct.pack("<5Q", *sources))
    e.write(system.addr + 0x38, struct.pack("<5I", 3, 4, 5, 6, 7))
    for i in range(11):
        path = system.addr + 0xE8 + 0x30 * i
        present = i not in (0, 4)
        e.write(path, struct.pack("<5Q", f.files.refs.make("names file %d" % i), 0, 0, 1 if present else 0, 0))
    f.files.files["personal_total.bin"] = personal_data(t.rng)
    f.files.files["learnset 0"] = learnset_file(t.rng)
    for pms in (bytes(t.rng.randrange(256) for _ in range(0x123)), b"", None):
        if pms is None:
            f.files.files.pop("pms_total.bin", None)
        else:
            f.files.files["pms_total.bin"] = pms
        t.emu.write(res.stack + 0x80, struct.pack("<Q", 0))
        t.call(INITIALIZE, res.addr)
        owners = e.read(OWNERS["unk cache"], 0x20 * len(OWNERS))
        t.check((e.read(PERSONAL_SOURCE, 0x30), owners, e.read(NAMES, 0x60)))
        pms_ptr = e.read_u64(OWNERS["pms"])
        t.check(e.read(pms_ptr, 0x123) if pms_ptr else None)
        no_species = e.read_u64(OWNERS["no species moves"])
        t.check(learnset_state(t, no_species))
        t.call(FINALIZE)
        t.check((e.read(PERSONAL_SOURCE, 0x30), e.read(OWNERS["unk cache"], 0x20 * len(OWNERS)), e.read(NAMES, 0x60)))
    t.check((res.state(), f.log))


PT_INIT_GLOBALS, PT_DESTROY_DATA, INIT_GLOBALS, CXA_ATEXIT = 0x710071E120, 0x710071D7D0, 0x710071E2C0, 0x71000001C0
# the system's destructors (registered at startup) and the global each destroys
DESTROYERS = [(0x710071E150, NAME_BUFFER), (0x710071E190, 0), (0x710071E4F0, OWNERS["unk cache"]),
              (0x710071E550, OWNERS["personal"]), (0x710071E550, OWNERS["growth personal"]),
              (0x710071E5B0, OWNERS["growth"]), (0x710071E610, OWNERS["growth cache"]),
              (0x710071E670, OWNERS["level-up moves"]), (0x710071E670, OWNERS["no species moves"]),
              (0x710071E670, OWNERS["unk learnset"]), (0x710071E6C0, OWNERS["learnset cache"]),
              (0x710071E720, OWNERS["evolutions"]), (0x710071E780, OWNERS["evolution cache"]),
              (0x710071E7E0, OWNERS["mega check"]), (0x710071E7E0, OWNERS["mega evolutions"]),
              (0x710071E840, OWNERS["mega cache"]), (0x710071E8A0, OWNERS["pms"])]
GLOBALS_START, GLOBALS_SIZE = 0x710163EA68, 0x710163ECD8 - 0x710163EA68


def test_personal_globals(t):
    registered = []
    t.emu.stub_function(CXA_ATEXIT, "__cxa_atexit", lambda e: registered.append((e.reg(0), e.reg(1), e.reg(2))) or 0)
    t.emu.write(GLOBALS_START, bytes(t.rng.randrange(256) for _ in range(GLOBALS_SIZE)))
    t.call(PT_INIT_GLOBALS)
    t.call(INIT_GLOBALS)
    t.check((t.emu.read(GLOBALS_START, GLOBALS_SIZE), registered))
    # real tables to destroy, as Initialize makes them (with the species data loaded)
    t.emu.write(PERSONAL_SOURCE, bytes(8))
    res, f, system = fakes.FakeResource(t.emu), FAKES[0], SYSTEM[0]
    t.emu.write(system.addr + 0x38, struct.pack("<5I", 2, 2, 2, 2, 2))
    for i in range(11):
        t.emu.write(system.addr + 0xE8 + 0x30 * i, struct.pack("<5Q", f.files.refs.make("names %d" % i), 0, 0, 1, 0))
    f.files.files["personal_total.bin"] = personal_data(t.rng)
    f.files.files["pms_total.bin"] = b"pms"
    t.call(INITIALIZE, res.addr)
    order = list(DESTROYERS)
    t.rng.shuffle(order)
    for destroy, obj in order:
        t.call(destroy, obj)
        t.check(t.emu.read(GLOBALS_START, GLOBALS_SIZE))
    t.call(PT_DESTROY_DATA, PERSONAL_DATA)
    t.call(PT_DESTROY_DATA, PERSONAL_DATA)
    t.check((t.emu.read(GLOBALS_START, GLOBALS_SIZE), res.state(), f.log))
