"""NintendoWare layouts (.bflyt), the menus' screens inside the .arc archives: list their panes and change where a pane
sits, its size, turn, scale, alpha and whether it shows (docs/PLAN.md, roadmap phase 8). The changes are made in
place, so the rest of the file stays as it was.

Layout (little endian, version 8.6 in Let's Go): "FLYT", u16 0xFEFF, u16 header size (0x14), u32 version, u32 file
size, u16 section count, u16 0, then the sections, each a magic, a u32 size (header included) and its body:
  lyt1 the screen (u8 drawn from the centre, f32 width and height), txl1 texture names, fnl1 font names, mat1
       materials, pan1/pic1/txt1/wnd1/prt1/bnd1/ali1/scr1 panes, pas1/pae1 around a pane's children, grp1 groups,
       grs1/gre1 around a group's children, usd1 the user data of the pane before it
  a pane (0x54 bytes before its kind's own fields):
    0x08 u8 flags (bit 0 shown), 0x09 u8 origin, 0x0A u8 alpha, 0x0B u8 more flags,
    0x0C name (24 bytes, zero-padded), 0x24 user info (8 bytes), 0x2C f32 x, y, z, 0x38 f32 turn about x, y, z
    (degrees), 0x44 f32 scale x, y, 0x4C f32 width, height
  txt1 after that: u16 buffer bytes, u16 text bytes, u16 material, u16 font, u8 position, u8 alignment, u8 flags,
    u8 0, f32 italic ratio, u32 text offset (from the section), u32 top and bottom colours (RGBA), f32 font width
    and height, ...; menus fill most text boxes from their message file when they open.

Animations (.bflan) key some of these values (the main menu's opening slides each button along x), and a value an
animation keys is overwritten while it plays: check with `bflyt.py animated`.

  import bflyt
  layout = bflyt.Layout(data)
  pane = layout.pane("N_manu_02"); pane.set(y=-140, scale=(1.2, 1.2))
  data = layout.data                          # the changed file

  python -P tools/bflyt.py panes <file.bflyt | file.arc:path/in.bflyt>
  python -P tools/bflyt.py set <in> <out.bflyt> <pane> key=value...     x y z turn_x turn_y turn_z scale_x scale_y
                                                                       width height alpha shown (0 or 1)
  python -P tools/bflyt.py animated <file.bflan | file.arc:path/in.bflan>...   the panes and values each animates
  python -P tools/bflyt.py check [files.arc...]   walk every layout and animation in the archives (default: every
                                                  .arc in orig/romfs)
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sarc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PANES = ("pan1", "pic1", "txt1", "wnd1", "prt1", "bnd1", "ali1", "scr1")
# The pane values set() takes: (offset in the pane, format)
FIELDS = {
    "x": (0x2C, "f"), "y": (0x30, "f"), "z": (0x34, "f"),
    "turn_x": (0x38, "f"), "turn_y": (0x3C, "f"), "turn_z": (0x40, "f"),
    "scale_x": (0x44, "f"), "scale_y": (0x48, "f"),
    "width": (0x4C, "f"), "height": (0x50, "f"),
    "alpha": (0x0A, "B"),
}
# What an animation's FLPA (pane) targets are, by index
PANE_TARGETS = ("x", "y", "z", "turn_x", "turn_y", "turn_z", "scale_x", "scale_y", "width", "height")


class LayoutError(ValueError):
    pass


def sections(data, magic):
    if data[:4] != magic:
        raise LayoutError("not a %s file" % magic.decode())
    bom, hsize = struct.unpack_from("<HH", data, 4)
    size, count = struct.unpack_from("<IH", data, 0x0C)
    if bom != 0xFEFF or size != len(data):
        raise LayoutError("not a little-endian %s of its own size" % magic.decode())
    p = hsize
    for _ in range(count):
        if p + 8 > len(data):
            raise LayoutError("a section runs past the end")
        kind = bytes(data[p:p + 4]).decode("ascii", "replace")
        length = struct.unpack_from("<I", data, p + 4)[0]
        if length < 8 or p + length > len(data):
            raise LayoutError("section %s at %#x runs past the end" % (kind, p))
        yield kind, p, length
        p += length
    if p != len(data):
        raise LayoutError("the sections end at %#x of %#x" % (p, len(data)))


class Pane:
    def __init__(self, layout, kind, at, depth, parent):
        self.layout, self.kind, self.at, self.depth, self.parent = layout, kind, at, depth, parent
        self.name = bytes(layout.data[at + 0x0C:at + 0x24]).split(b"\0")[0].decode("ascii")

    def get(self, key):
        if key == "shown":
            return self.layout.data[self.at + 8] & 1
        offset, fmt = FIELDS[key]
        return struct.unpack_from("<" + fmt, self.layout.data, self.at + offset)[0]

    def set(self, **values):
        """Change values: the keys of FIELDS, shown (0 or 1), or scale / size / position as tuples."""
        for key, value in values.items():
            if key in ("scale", "size", "position"):
                names = {"scale": ("scale_x", "scale_y"), "size": ("width", "height"), "position": ("x", "y", "z")}[key]
                self.set(**dict(zip(names, value)))
            elif key == "shown":
                self.layout.data[self.at + 8] = (self.layout.data[self.at + 8] & ~1) | (1 if value else 0)
            elif key in FIELDS:
                offset, fmt = FIELDS[key]
                if fmt == "B" and not 0 <= int(value) <= 255:
                    raise LayoutError("%s is 0 to 255" % key)
                struct.pack_into("<" + fmt, self.layout.data, self.at + offset, int(value) if fmt == "B" else float(value))
            else:
                raise LayoutError("a pane has no value %s; it has %s and shown" % (key, ", ".join(FIELDS)))

    def text(self):
        """A text box's own text (most are filled in by the game), or None for other panes."""
        if self.kind != "txt1":
            return None
        length = struct.unpack_from("<H", self.layout.data, self.at + 0x56)[0]
        offset = struct.unpack_from("<I", self.layout.data, self.at + 0x64)[0]
        if not length or not offset:
            return ""
        return bytes(self.layout.data[self.at + offset:self.at + offset + length]).decode("utf-16-le").rstrip("\0")

    def describe(self):
        g = self.get
        s = "%s%s %-24s at (%g, %g, %g) size %g x %g" % ("  " * self.depth, self.kind, self.name, g("x"), g("y"), g("z"),
                                                       g("width"), g("height"))
        if (g("scale_x"), g("scale_y")) != (1, 1):
            s += " scale (%g, %g)" % (g("scale_x"), g("scale_y"))
        if (g("turn_x"), g("turn_y"), g("turn_z")) != (0, 0, 0):
            s += " turned (%g, %g, %g)" % (g("turn_x"), g("turn_y"), g("turn_z"))
        if g("alpha") != 255:
            s += " alpha %d" % g("alpha")
        if not g("shown"):
            s += " hidden"
        if self.text():
            s += " text %r" % self.text()
        return s


class Layout:
    def __init__(self, data):
        self.data = bytearray(data)
        self.panes = []
        depth, last, parents = 0, None, []
        for kind, at, _ in sections(self.data, b"FLYT"):
            if kind in PANES:
                last = Pane(self, kind, at, depth, parents[-1] if parents else None)
                self.panes.append(last)
            elif kind == "pas1":
                if last is None:
                    raise LayoutError("children before any pane")
                parents.append(last)
                depth += 1
            elif kind == "pae1":
                if not parents:
                    raise LayoutError("more pane ends than starts")
                parents.pop()
                depth -= 1
        if parents:
            raise LayoutError("a pane's children never end")

    def pane(self, name):
        found = [p for p in self.panes if p.name == name]
        if not found:
            raise LayoutError("the layout has no pane %s" % name)
        return found[0]


def animated(data):
    """What a .bflan animates: [(pane or material name, is a material, tag, [target names])]."""
    out = []
    for kind, at, _ in sections(data, b"FLAN"):
        if kind != "pai1":
            continue
        count, entries = struct.unpack_from("<HI", data, at + 0x0E)
        for o in struct.unpack_from("<%dI" % count, data, at + entries):
            e = at + o
            name = bytes(data[e:e + 28]).split(b"\0")[0].decode("ascii")
            ntags, material = struct.unpack_from("<BB", data, e + 28)
            for to in struct.unpack_from("<%dI" % ntags, data, e + 32):
                t = e + to
                tag = bytes(data[t:t + 4]).decode("ascii")
                targets = []
                for io in struct.unpack_from("<%dI" % data[t + 4], data, t + 8):
                    target = data[t + io + 1]
                    targets.append(PANE_TARGETS[target] if tag == "FLPA" and target < len(PANE_TARGETS) else str(target))
                out.append((name, bool(material), tag, targets))
    return out


def load(spec):
    """A file's bytes; file.arc:path names a file inside an archive."""
    if ".arc:" in spec:
        path, name = spec.split(".arc:", 1)
        return sarc.find(sarc.read(open(path + ".arc", "rb").read()), name)
    return open(spec, "rb").read()


def check(paths):
    layouts = animations = panes = 0
    for path in paths:
        for name, data in sarc.read(open(path, "rb").read()):
            try:
                if name.endswith(".bflyt"):
                    layout = Layout(data)
                    panes += len(layout.panes)
                    layouts += 1
                elif name.endswith(".bflan"):
                    animated(data)
                    animations += 1
            except (LayoutError, struct.error, UnicodeDecodeError) as e:
                print("FAIL %s:%s: %s" % (path, name, e))
                return False
    print("%d layouts (%d panes) and %d animations in %d archives walked" % (layouts, panes, animations, len(paths)))
    return True


def number(text):
    return float(text) if any(c in text for c in ".eE") else int(text)


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "panes":
            for pane in Layout(load(argv[2])).panes:
                print(pane.describe())
        elif len(argv) >= 6 and argv[1] == "set":
            layout = Layout(load(argv[2]))
            values = {}
            for item in argv[5:]:
                key, _, value = item.partition("=")
                values[key] = number(value)
            layout.pane(argv[4]).set(**values)
            open(argv[3], "wb").write(layout.data)
        elif len(argv) >= 3 and argv[1] == "animated":
            for spec in argv[2:]:
                print("==", spec)
                for name, material, tag, targets in animated(load(spec)):
                    print("  %s%s %s %s" % (name, " (material)" if material else "", tag, " ".join(targets)))
        elif len(argv) >= 2 and argv[1] == "check":
            paths = argv[2:] or sorted(glob.glob(os.path.join(ROOT, "orig", "romfs", "**", "*.arc"), recursive=True))
            return 0 if check(paths) else 1
        else:
            print(__doc__)
            return 2
    except (LayoutError, sarc.SarcError, OSError, ValueError) as e:
        print("bflyt: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
