"""decomp/src/field/placement.cpp against the originals (decomp/tools/difftest.py): the zone lookups in
the placement files the field reads, on made-up placement files."""
import struct

PLACEMENT_ROOT = 0x7100132900
FIND_ZONE = 0x7100132960
FIND_ZONE_AND_AREA = 0x7100132AA0
REQUEST = 0x110          # a gfl::fs::ReadRequest
FILE_DATA = 0x170        # where a read file's bytes are
SENTINEL = 0xA5A5A5A5A5A5A5A5


class Buffer:
    """A made-up FlatBuffer laid out front to back: each table just after its vtable, references
    pointing forward, patched once everything is placed."""

    def __init__(self):
        self.b = bytearray()
        self.refs = []
        self.at = {}

    def align(self, n):
        while len(self.b) % n:
            self.b.append(0)

    def ref(self, label):
        self.refs.append((len(self.b), label))
        self.b += b"\0" * 4

    def table(self, label, vtable, body, align=8):
        """vtable: its size and the field offsets (the table's size is filled in); body: a function
        that writes the table's fields after its soffset."""
        self.align(2)
        vt = len(self.b)
        size_at = vt + 2
        self.b += struct.pack("<HH", vtable[0], 0)
        self.b += b"".join(struct.pack("<H", o) for o in vtable[1:])
        self.align(align)
        start = len(self.b)
        self.at[label] = start
        self.b += struct.pack("<i", start - vt)
        body()
        self.align(4)
        struct.pack_into("<H", self.b, size_at, len(self.b) - start)

    def finish(self):
        for pos, label in self.refs:
            struct.pack_into("<I", self.b, pos, self.at[label] - pos)
        return bytes(self.b)


def placement_file(area, zones):
    """A placement file: area is the root's field 1 (None for a vtable without it, 0 for a slot of 0);
    zones are (kind, name): kind "named" (field 1 holds the name), "short" (a vtable too short for
    field 1), "unset" (field 1's slot is 0) or "long" (a vtable of 17 slots, as the game's have)."""
    buf = Buffer()
    buf.ref("root")

    def root_body():
        buf.ref("zones")
        if area:
            buf.b += struct.pack("<Q", area)

    vt = [6, 4] if area is None else [8, 4, 0] if area == 0 else [8, 4, 8]
    buf.table("root", vt, root_body)
    buf.align(4)
    buf.at["zones"] = len(buf.b)
    buf.b += struct.pack("<I", len(zones))
    for i in range(len(zones)):
        buf.ref("zone%d" % i)
    for i in range(len(zones)):
        buf.table("zone%d" % i, [6, 4], lambda i=i: buf.ref("info%d" % i))
    for i, (kind, name) in enumerate(zones):
        def info_body(kind=kind, name=name):
            if kind in ("named", "long"):
                buf.b += b"\0" * 4
                buf.b += struct.pack("<Q", name)
        vt = {"named": [8, 0, 8], "short": [6, 0], "unset": [8, 0, 0], "long": [0x26, 0, 8] + [0] * 15}[kind]
        buf.table("info%d" % i, vt, info_body)
    return buf.finish(), buf.at


def field_with(t, files):
    """The field's object with a read request for each placement file. Returns it and a map from the
    addresses of the files' roots and zones to their names."""
    e = t.emu
    names = {}
    requests = e.alloc(max(REQUEST * len(files), 1))
    for i, (area, zones) in enumerate(files):
        data, at = placement_file(area, zones)
        base = e.alloc(data, align=16)
        file = e.alloc(0x180)
        e.write(file + FILE_DATA, struct.pack("<Q", base))
        e.write(requests + REQUEST * i + 8, struct.pack("<Q", file))
        names[base + at["root"]] = "file%d" % i
        for j in range(len(zones)):
            names[base + at["zone%d" % j]] = "file%d.zone%d" % (i, j)
    field = e.alloc(0x548)
    end = requests + REQUEST * len(files)
    e.write(field + 0x530, struct.pack("<QQQ", requests, end, end))
    return field, names


def label(names, addr):
    return names.get(addr, "0x%x" % addr if addr else None)


def lookups(t, files, wanted):
    field, names = field_with(t, files)
    for i in range(len(files)):
        t.check(label(names, t.call(PLACEMENT_ROOT, field, i)))
    out = t.emu.alloc(8)
    for name in wanted:
        t.check(label(names, t.call(FIND_ZONE, field, name)))
        t.emu.write(out, struct.pack("<Q", SENTINEL))
        t.check((label(names, t.call(FIND_ZONE_AND_AREA, field, name, out)), t.emu.read_u64(out)))


FIXED = [
    (0x0F704310242F5D17, [("named", 0x1111), ("long", 0x2222), ("named", 0x3333)]),
    (None, [("short", 0), ("named", 0x2222)]),
    (0, [("unset", 0), ("named", 0x4444)]),
    (0x9999, []),
    (0x7777, [("named", 0x4444), ("long", 0)]),
]


def test_zone_lookups_fixed(t):
    lookups(t, FIXED, [0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0, SENTINEL])


def test_zone_lookups_no_files(t):
    lookups(t, [], [0x1111, 0])


def test_zone_lookups_unnamed_first(t):
    # A zone without a name matches 0 wherever it is, so the first file's unnamed zone is found for 0.
    lookups(t, [(0x1234, [("named", 5), ("unset", 0)]), (0x5678, [("short", 0)])], [0, 5, 6])


def test_zone_lookups_random(t):
    kinds = ["named", "named", "long", "short", "unset"]
    for _ in range(40):
        files = []
        pool = [t.rng.getrandbits(64) for _ in range(6)] + [t.rng.randrange(1, 16)]
        for _ in range(t.rng.randrange(1, 6)):
            area = t.rng.choice([None, 0, t.rng.getrandbits(64)])
            zones = []
            for _ in range(t.rng.randrange(0, 7)):
                kind = t.rng.choice(kinds)
                zones.append((kind, t.rng.choice(pool) if kind in ("named", "long") else 0))
            files.append((area, zones))
        lookups(t, files, pool + [0, t.rng.getrandbits(64)])
