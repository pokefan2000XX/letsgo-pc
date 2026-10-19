"""tools/flatbuf.py on made-up tables: every kind of field, building a file and copying it back to the same nodes, aligned
scalars, a list made longer after the file's end with the rest left as it was, and the mistakes it refuses.

  python -P tests/smoke/flatbuf_tool.py      (prints PASS or what failed)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import flatbuf  # noqa: E402

POINT = {0: "f32", 1: "f32", 2: "u64"}
ITEM = {0: ("table", POINT), 1: "i32", 2: "u8", 3: "string"}
FILE = {0: ("tables", ITEM), 1: "u32", 2: "i64", 3: "string", 4: ("table", POINT)}


def point(x, z, name):
    return {0: ("f32", x), 1: ("f32", z), 2: ("u64", name)}


def item(n):
    # field 2 (u8) is left out of the even items, as a file leaves out fields that are 0
    node = {0: ("table", point(1.5 * n, -2.0, 0xCBF29CE484222645 + n)), 1: ("i32", -n), 3: ("string", "item %d" % n)}
    if n % 2:
        node[2] = ("u8", n)
    return node


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def aligned(table, schema):
    """Every scalar sits on a multiple of its size, down the tables."""
    for field, kind in schema.items():
        at = table.slot(field)
        if at is None:
            continue
        if isinstance(kind, tuple):
            subs = [table.table(field)] if kind[0] == "table" else table.tables(field)
            if not all(aligned(t, kind[1]) for t in subs):
                return False
        elif kind in flatbuf.SIZES and at % flatbuf.SIZES[kind]:
            return False
    return True


def main():
    node = {0: ("tables", [item(n) for n in range(3)]), 1: ("u32", 0xFFFFFFFF), 2: ("i64", -(1 << 62)),
            3: ("string", "a made-up file"), 4: ("table", point(0.0, 9.0, 7))}
    data = flatbuf.build(node)
    root = flatbuf.root(data)
    check(flatbuf.copy(root, FILE) == node, "a built file copies back to the same nodes")
    check(root.tables(0)[1].scalar(2, "u8") == 1 and root.tables(0)[0].slot(2) is None, "a field left out reads as 0")
    check(root.string(3) == "a made-up file" and root.tables(0)[2].table(0).scalar(0, "f32") == 3.0, "strings and floats")
    check(aligned(root, FILE), "scalars are aligned")
    check(flatbuf.build(flatbuf.copy(root, FILE)) == data, "built again, the same bytes")

    # A longer list, written after the end; the file before it stays as it was.
    longer = flatbuf.copy(root, FILE)[0][1] + [item(3)]
    w = flatbuf.Writer(data)
    w.point(root.slot(0), w.tables(longer))
    grown = bytes(w.buf)
    check(grown[:root.slot(0)] == data[:root.slot(0)] and grown[root.slot(0) + 4:len(data)] == data[root.slot(0) + 4:],
          "only the list's offset changes in the old bytes")
    back = flatbuf.copy(flatbuf.root(grown), FILE)
    check(back[0][1] == longer and {k: v for k, v in back.items() if k} == {k: v for k, v in node.items() if k},
          "the list has the new item and the other fields are the same")
    check(aligned(flatbuf.root(grown), FILE), "scalars written after the end are aligned too")

    # Mistakes are refused.
    try:
        flatbuf.copy(root, {k: v for k, v in FILE.items() if k != 2})
        check(False, "a field the schema doesn't know is refused")
    except flatbuf.FlatError:
        pass
    try:
        flatbuf.Writer(data).point(8, 4)
        check(False, "an offset pointing back is refused")
    except flatbuf.FlatError:
        pass
    print("PASS")


if __name__ == "__main__":
    main()
