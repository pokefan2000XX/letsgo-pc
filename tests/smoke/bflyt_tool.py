"""tools/bflyt.py on a made-up layout and animation: panes with their depth and parent, a text box's own text, values
changed in place with the rest of the file left as it was, what an animation keys, and the mistakes it refuses.

  python -P tests/smoke/bflyt_tool.py      (prints PASS or what failed)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import bflyt  # noqa: E402


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def refuses(what, f, *args, **kwargs):
    try:
        f(*args, **kwargs)
    except bflyt.LayoutError:
        return
    print("FAIL accepted " + what)
    sys.exit(1)


def section(magic, body):
    return magic + struct.pack("<I", 8 + len(body)) + body


def pane(magic, name, at=(0.0, 0.0), size=(10.0, 10.0), extra=b""):
    base = struct.pack("<BBBB", 1, 0, 255, 0) + name.encode().ljust(24, b"\0") + bytes(8)
    base += struct.pack("<3f3f2f2f", at[0], at[1], 0, 0, 0, 0, 1, 1, size[0], size[1])
    return section(magic, base + extra)


def text_box(name, text):
    units = (text + "\0").encode("utf-16-le")
    fields = struct.pack("<HHHHBBBBfI", len(units), len(units), 0, 0, 0, 0, 0, 0, 0.0, 0x88) + bytes(0x88 - 0x68)
    return pane(b"txt1", name, extra=fields + units + bytes(-len(units) % 4))


def file(magic, sections):
    body = b"".join(sections)
    return magic + struct.pack("<HHIIHH", 0xFEFF, 0x14, 0x08060000, 0x14 + len(body), len(sections), 0) + body


def layout():
    return file(b"FLYT", [section(b"lyt1", struct.pack("<B3xff", 1, 1280, 720)), pane(b"pan1", "RootPane", size=(1280, 720)),
                          section(b"pas1", b""), pane(b"pic1", "P_icon_00", at=(-208, -180)), text_box("T_text_00", "Hi"),
                          section(b"pae1", b""), section(b"usd1", bytes(4))])


def animation():
    # pai1: frames, loop, textures, one entry (P_icon_00) with an FLPA tag keying x and scale_y and an FLVI tag
    def info(target):
        return struct.pack("<BBBxHxxI", 0, target, 2, 1, 12) + struct.pack("<fff", 0, 1, 0)
    infos = [info(0), info(7)]
    flpa = b"FLPA" + struct.pack("<B3x", 2) + struct.pack("<II", 16, 16 + len(infos[0])) + b"".join(infos)
    flvi = b"FLVI" + struct.pack("<B3xI", 1, 12) + info(0)
    entry = b"P_icon_00".ljust(28, b"\0") + struct.pack("<BBxx", 2, 0) + struct.pack("<II", 40, 40 + len(flpa)) + flpa + flvi
    body = struct.pack("<HBxHHI", 10, 0, 0, 1, 20) + struct.pack("<I", 24) + entry
    return file(b"FLAN", [section(b"pat1", bytes(8)), section(b"pai1", body)])


def main():
    data = layout()
    lay = bflyt.Layout(data)
    check([(p.kind, p.name, p.depth) for p in lay.panes] == [("pan1", "RootPane", 0), ("pic1", "P_icon_00", 1), ("txt1", "T_text_00", 1)],
          "panes, kinds and depths")
    icon = lay.pane("P_icon_00")
    check(icon.parent.name == "RootPane" and lay.pane("RootPane").parent is None, "parents")
    check((icon.get("x"), icon.get("y"), icon.get("width"), icon.get("alpha"), icon.get("shown")) == (-208, -180, 10, 255, 1), "values")
    check(lay.pane("T_text_00").text() == "Hi" and icon.text() is None, "a text box's own text")

    icon.set(y=-144, scale=(1.25, 1.25), alpha=128, shown=0, turn_z=-45)
    check((icon.get("y"), icon.get("scale_x"), icon.get("scale_y"), icon.get("alpha"), icon.get("shown"), icon.get("turn_z")) ==
          (-144, 1.25, 1.25, 128, 0, -45), "values changed")
    changed = [i for i in range(len(data)) if data[i] != lay.data[i]]
    at = icon.at
    check(len(lay.data) == len(data) and all(at + 8 <= i < at + 0x54 for i in changed), "only the pane's bytes changed")
    check(bflyt.Layout(bytes(lay.data)).pane("P_icon_00").get("y") == -144, "the changed file reads back")
    check("scale (1.25, 1.25)" in icon.describe() and "hidden" in icon.describe(), "described")

    check(bflyt.animated(animation()) == [("P_icon_00", False, "FLPA", ["x", "scale_y"]), ("P_icon_00", False, "FLVI", ["0"])],
          "what an animation keys")

    refuses("a value panes don't have", icon.set, colour=3)
    refuses("an alpha past 255", icon.set, alpha=300)
    refuses("a pane the layout hasn't got", lay.pane, "N_none")
    refuses("a pane end without a start", bflyt.Layout, file(b"FLYT", [pane(b"pan1", "RootPane"), section(b"pae1", b"")]))
    refuses("children that never end", bflyt.Layout, file(b"FLYT", [pane(b"pan1", "RootPane"), section(b"pas1", b"")]))
    refuses("a file of another size", bflyt.Layout, data + bytes(4))
    refuses("an animation as a layout", bflyt.Layout, animation())
    print("PASS")


if __name__ == "__main__":
    main()
