"""FlatBuffers without a schema compiler: read a file's tables, copy them into nodes you can change, and write nodes
after the end of a file, so it gains a table or a longer list without being rebuilt (hacks/new-cutscene/make_romfs.py
adds a trigger to a placement file this way).

A table starts with a signed 32-bit offset back to its vtable: u16 vtable size, u16 table size, then a u16 per field,
the field's offset in the table, 0 for a field the table leaves out (it reads as 0). A field holding a table, a list
(vector) or a string holds an unsigned 32-bit offset from itself to it, so what it points at must come after it. A list
is a u32 count and then an offset per item; a string a u32 length, the UTF-8 bytes and a 0.

A node is a dict {field: (kind, value)}: kind "u8", "i32", "u32", "f32", "u64" or "i64" with a number, "string" with a
str, "table" with a node, or "tables" with a list of nodes. A schema says which kinds a table's fields have:
{field: kind}, with ("table", schema) or ("tables", schema) for the fields that hold tables.

  import flatbuf
  root = flatbuf.root(data)                 # the root table
  node = flatbuf.copy(root, schema)         # it as a node, down its tables
  data = flatbuf.build(node)                # a new file with that node as its root
  w = flatbuf.Writer(data); pos = w.tables(nodes); w.point(slot, pos)   # a longer list, written after the end
"""
import struct

FORMATS = {"u8": "B", "i32": "i", "u32": "I", "f32": "f", "u64": "Q", "i64": "q"}
SIZES = {"u8": 1, "i32": 4, "u32": 4, "f32": 4, "u64": 8, "i64": 8}


class FlatError(ValueError):
    pass


class Table:
    """A table in a buffer: its fields by their vtable."""

    def __init__(self, buf, pos):
        self.buf, self.pos = buf, pos
        vt = pos - struct.unpack_from("<i", buf, pos)[0]
        vsize = struct.unpack_from("<H", buf, vt)[0]
        self.slots = list(struct.unpack_from("<%dH" % ((vsize - 4) // 2), buf, vt + 4))

    def slot(self, field):
        """Where the field is in the buffer, or None when the table leaves it out."""
        return self.pos + self.slots[field] if field < len(self.slots) and self.slots[field] else None

    def has(self):
        return [f for f, off in enumerate(self.slots) if off]

    def scalar(self, field, kind):
        at = self.slot(field)
        return struct.unpack_from("<" + FORMATS[kind], self.buf, at)[0] if at is not None else 0

    def _target(self, at):
        return at + struct.unpack_from("<I", self.buf, at)[0]

    def table(self, field):
        at = self.slot(field)
        return Table(self.buf, self._target(at)) if at is not None else None

    def tables(self, field):
        at = self.slot(field)
        if at is None:
            return []
        v = self._target(at)
        return [Table(self.buf, self._target(v + 4 + 4 * i)) for i in range(struct.unpack_from("<I", self.buf, v)[0])]

    def string(self, field):
        at = self.slot(field)
        if at is None:
            return None
        s = self._target(at)
        return bytes(self.buf[s + 4:s + 4 + struct.unpack_from("<I", self.buf, s)[0]]).decode("utf-8")


def root(buf):
    return Table(buf, struct.unpack_from("<I", buf, 0)[0])


def copy(table, schema):
    """The table as a node, with the fields it has, down the tables the schema names. A field the schema doesn't know
    is refused, since leaving it out would change the file."""
    unknown = [f for f in table.has() if f not in schema]
    if unknown:
        raise FlatError("a table has fields %s that the schema doesn't know" % unknown)
    node = {}
    for field, kind in schema.items():
        if table.slot(field) is None:
            continue
        if isinstance(kind, tuple):
            what, sub = kind
            node[field] = (what, copy(table.table(field), sub) if what == "table" else [copy(t, sub) for t in table.tables(field)])
        elif kind == "string":
            node[field] = (kind, table.string(field))
        else:
            node[field] = (kind, table.scalar(field, kind))
    return node


class Writer:
    """Writes nodes after the end of a buffer: each table after its vtable (vtables are shared when they're the same)
    and everything a table points at after the table, so every offset points forward."""

    def __init__(self, buf=b""):
        self.buf = bytearray(buf)
        self.vtables = {}

    def pad(self, n):
        while len(self.buf) % n:
            self.buf.append(0)

    def point(self, slot, target):
        """Makes the offset at `slot` point at `target` (which must come after it)."""
        if target <= slot:
            raise FlatError("an offset can only point forward")
        struct.pack_into("<I", self.buf, slot, target - slot)

    def node(self, node):
        """Writes a table and what it points at; returns where the table is."""
        size = lambda kind: SIZES.get(kind, 4)
        offsets, at = {}, 4
        for field, (kind, _) in sorted(node.items(), key=lambda item: (-size(item[1][0]), item[0])):
            s = size(kind)
            at = (at + s - 1) // s * s
            offsets[field] = at
            at += s
        length = (at + 3) // 4 * 4
        count = max(node) + 1 if node else 0
        vtable = struct.pack("<HH%dH" % count, 4 + 2 * count, length, *[offsets.get(f, 0) for f in range(count)])
        if vtable not in self.vtables:
            self.pad(2)
            self.vtables[vtable] = len(self.buf)
            self.buf += vtable
        self.pad(8)
        pos = len(self.buf)
        self.buf += bytes(length)
        struct.pack_into("<i", self.buf, pos, pos - self.vtables[vtable])
        later = []
        for field, (kind, value) in node.items():
            if kind in FORMATS:
                struct.pack_into("<" + FORMATS[kind], self.buf, pos + offsets[field], value)
            else:
                later.append((pos + offsets[field], kind, value))
        for slot, kind, value in later:
            if kind == "table":
                self.point(slot, self.node(value))
            elif kind == "tables":
                self.point(slot, self.tables(value))
            elif kind == "string":
                self.point(slot, self.string(value))
            else:
                raise FlatError("no such kind: %s" % kind)
        return pos

    def tables(self, nodes):
        """Writes a list of tables; returns where the list is."""
        self.pad(4)
        pos = len(self.buf)
        self.buf += struct.pack("<I", len(nodes)) + bytes(4 * len(nodes))
        for i, item in enumerate(nodes):
            self.point(pos + 4 + 4 * i, self.node(item))
        return pos

    def string(self, text):
        self.pad(4)
        pos = len(self.buf)
        data = text.encode("utf-8")
        self.buf += struct.pack("<I", len(data)) + data + b"\0"
        return pos


def build(node):
    """A new file with the node as its root table."""
    w = Writer(bytes(4))
    w.point(0, w.node(node))
    w.pad(8)
    return bytes(w.buf)
