"""decomp/src/item/item.cpp against the originals: item objects made, loaded (from fake files),
copied and destroyed, their parameters and effects on random records, the per-item flags, and the
item lists over every ID."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fakes  # noqa: E402

ITEM_CLEAR, ITEM_LOAD_CTOR, ITEM_LOAD, ITEM_COPY = 0x710071C420, 0x710071C4D0, 0x710071C760, 0x710071C910
ITEM_DTORS = [(0x710071C720, False), (0x710071C730, True), (0x710071C740, False), (0x710071C750, True)]
GET_PARAM, GET_EFFECT, IS_USED_ON_MOVE = 0x710071C930, 0x710071CC60, 0x710071CE30
LOAD_FLAGS, UNLOAD_FLAGS = 0x710071CEC0, 0x710071D180
FLAGS_LOW, FLAG6, FLAG7 = 0x710071D1B0, 0x710071D1E0, 0x710071D210
FLAGS = 0x710163EA48                                   # item::g_flags (a UniquePtr)
ITEM_PATH, FLAGS_PATH = 0x710073B880, 0x710073B950
# by item (u16): ball ID, IsMachine, machine index, machine move, IsBerry, IsMegaStone, IsGem,
# IsZCrystal, IsHeldZCrystal, IsCandy, IsUnk121, with their results' widths
U16_CHECKS = [(0x710071D240, 0xFF), (0x710071D390, 0xFF), (0x710071D3F0, 0xFFFFFFFF), (0x710071D4A0, 0xFFFF),
              (0x710071D5A0, 0xFF), (0x710071D640, 0xFF), (0x710071D690, 0xFF), (0x710071D6B0, 0xFF),
              (0x710071D6F0, 0xFF), (0x710071D720, 0xFF), (0x710071D770, 0xFF)]
BALL_ITEM, MACHINE_MOVE_BY_INDEX, BERRY = 0x710071D360, 0x710071D560, 0x710071D610
U32 = 0xFFFFFFFF


def setup(emu):
    system = fakes.FakeSystem(emu)
    emu.write(system.addr + 0x320, struct.pack("<Q", 0x7101234500))      # the file device
    files = fakes.FakeFiles(emu)
    files.path(ITEM_PATH, lambda e: "item%03d.dat" % (e.reg(0) & U32))
    files.path(FLAGS_PATH, lambda e: "item flags")
    FILES[0] = files


FILES = [None]


def record(rng):
    """An item record: random bytes, often with a use kind that matters and few use flags set."""
    data = bytearray(rng.randrange(256) for _ in range(0x24))
    data[0xC] = rng.choice([0, 1, 1, 1, 2, rng.randrange(256)])
    if rng.random() < 0.7:                                  # sparse use flags, so each effect turns up
        data[0x10:0x17] = bytes(rng.choice([0, 0, 0, 1 << rng.randrange(8), 0x3F]) for _ in range(7))
        data[0x17:0x1D] = bytes(rng.choice([0, 0, 0, 0, 0, 1, 0xFF]) for _ in range(6))
    return bytes(data)


def test_item_objects(t):
    res = fakes.FakeResource(t.emu)
    files = FILES[0]
    for i in range(1, 60):
        if t.rng.random() < 0.7:
            files.files["item%03d.dat" % i] = record(t.rng) if t.rng.random() < 0.9 else b""
    for _ in range(40):
        obj = t.emu.alloc(b"\xcc" * 0x80)
        t.emu.write(res.stack + 0x80, struct.pack("<Q", t.rng.choice([0, 1])))
        t.emu.write(res.stack, struct.pack("<Q", res.addr))
        if t.rng.random() < 0.5:
            t.call(ITEM_CLEAR, obj, t.rng.choice([0, 1]))
        else:
            t.call(ITEM_LOAD_CTOR, obj, t.rng.randrange(70))
        t.check(t.emu.read(obj, 0x80))
        for _ in range(2):
            t.call(ITEM_LOAD, obj, t.rng.randrange(70))
            t.check(t.emu.read(obj, 0x80))
        other = t.emu.alloc(bytes(t.rng.randrange(256) for _ in range(0x80)))
        t.call(ITEM_COPY, obj, other)
        t.check(t.emu.read(obj, 0x80))
        dtor, thunk = t.rng.choice(ITEM_DTORS)
        t.call(dtor, obj + 0x48 if thunk else obj)
        t.check(t.emu.read(obj, 0x80))
    t.check(files.log)


def test_item_params(t):
    obj = t.emu.alloc(0x80)
    for _ in range(300):
        t.emu.write(obj + 0x54, record(t.rng) + struct.pack("<I", t.rng.getrandbits(32)))
        t.check([t.call(GET_PARAM, obj, p) & U32 for p in list(range(0x48)) + [0x100, U32]])
        t.check((t.call(GET_EFFECT, obj) & U32, t.call(IS_USED_ON_MOVE, obj) & 0xFF))


def test_item_flags(t):
    res = fakes.FakeResource(t.emu)
    files = FILES[0]
    for size in [0, 1, 3, 4, 0x421, 0x422, 0x423, 0x430, None]:
        if size is None:
            files.files.pop("item flags", None)
        else:
            files.files["item flags"] = bytes(t.rng.randrange(256) for _ in range(size))
        t.call(LOAD_FLAGS, res.addr)
        ptr, owner, length, align = struct.unpack("<4Q", t.emu.read(FLAGS, 0x20))
        t.check((owner == res.addr, length, align, t.emu.read(ptr, length) if ptr else None))
    flags = t.emu.alloc(bytes(t.rng.randrange(256) for _ in range(0x430)))
    t.emu.write(FLAGS, struct.pack("<Q", flags))
    for item in list(range(0x430)) + [0xFFFF, U32]:
        t.check((t.call(FLAGS_LOW, item, 11) & 0xFF, t.call(FLAGS_LOW, item, t.rng.choice([0, 10, 12])) & 0xFF,
                 t.call(FLAG6, item) & 0xFF, t.call(FLAG7, item) & 0xFF))
    t.call(UNLOAD_FLAGS)
    t.check(t.emu.read(FLAGS, 0x20))
    t.call(UNLOAD_FLAGS)
    t.check((t.emu.read(FLAGS, 0x20), res.state(), files.log))


def test_item_lists(t):
    for item in list(range(1200)) + [0xFFFF, 0x10000 + 1, 0x10000 + 656]:
        t.check([t.call(f, item) & mask for f, mask in U16_CHECKS])
    t.check([t.call(BALL_ITEM, ball) & 0xFFFF for ball in range(30)])
    t.check([t.call(MACHINE_MOVE_BY_INDEX, i) & 0xFFFF for i in range(107)])
    t.check([t.call(BERRY, i) & 0xFFFF for i in list(range(80)) + [0x100, 0x148]])


ITEM_INIT_GLOBALS, ITEM_DESTROY_FLAGS, CXA_ATEXIT = 0x710071D7A0, 0x710071C3F0, 0x71000001C0


def test_item_globals(t):
    registered = []
    t.emu.stub_function(CXA_ATEXIT, "__cxa_atexit", lambda e: registered.append((e.reg(0), e.reg(1), e.reg(2))) or 0)
    res = fakes.FakeResource(t.emu)
    t.emu.write(FLAGS, bytes(range(0x20)))
    t.call(ITEM_INIT_GLOBALS)
    t.check((t.emu.read(FLAGS, 0x20), registered))
    for _ in range(2):
        t.emu.write(FLAGS, struct.pack("<4Q", t.emu.alloc(0x10), res.addr, 0x10, 1))
        t.call(ITEM_DESTROY_FLAGS, FLAGS)
        t.check((t.emu.read(FLAGS, 0x20), res.state()))
        t.call(ITEM_DESTROY_FLAGS, FLAGS)
        t.check((t.emu.read(FLAGS, 0x20), res.state()))
