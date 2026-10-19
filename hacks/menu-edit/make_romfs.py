"""Makes the menu-edit hack's files for the game (roadmap phase 8's check): the main menu (X) with an edited layout, texture
and text, all on its Bag button. From your own copy of the game it writes, in hacks/menu-edit/romfs/ (which stays on your
PC, since the files are made from the game's):

- bin/app/main_menu/bin/main_menu.arc: the menu's archive (SARC) with two of its files changed:
  - blyt/main_menu_00.bflyt, the layout: the Bag button (pane N_manu_02) raised and made a quarter larger. The menu's
    opening animations slide the buttons along x, so a button's height and scale are free to change;
  - timg/__Combined.bntx, the textures: the Bag icon (main_menu_03^p, and main_menu_03_passive^p when it's picked) turned
    from yellow to purple;
- bin/message/English/common/main_menu.dat: the menu's text, with BAG renamed BACKPACK (the button shows it when picked).

The textures go through the studio's texture tools (`pokebdsp lgpe export --as dds` and `import`), so this needs
letsgostudio built (tools/studio.py says where it looks). The layout goes through tools/bflyt.py, the archive through
tools/sarc.py and the text through tools/msg.py.

  python -P hacks/menu-edit/make_romfs.py [--studio <pokebdsp.exe>]
  build/pc/letsgo.exe --mods hacks/menu-edit

Needs orig/romfs (tools/prepare.sh).
"""
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bflyt  # noqa: E402
import msg  # noqa: E402
import sarc  # noqa: E402
import studio  # noqa: E402

ARC = "bin/app/main_menu/bin/main_menu.arc"
LAYOUT = "blyt/main_menu_00.bflyt"
TEXTURES = "timg/__Combined.bntx"
BAG_PANE = "N_manu_02"          # the bottom row is N_manu_01 to 05: Pokédex, Bag, Party, Communicate, Save
RAISE, BIGGER = 36.0, 1.25
BAG_ICONS = ("main_menu_03^p", "main_menu_03_passive^p")
MESSAGES = "bin/message/English/common/main_menu.dat"
BAG_LABEL, BAG_TEXT = "main_menu_01_03", "BACKPACK"


def orig(path):
    return open(os.path.join(ROOT, "orig", "romfs", *path.split("/")), "rb").read()


def write(path, data):
    out = os.path.join(HERE, "romfs", *path.split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data)
    print("wrote", os.path.relpath(out, ROOT))


def turn(colour):
    """An RGB565 colour with red taking green's value, green blue's and blue red's: yellow becomes purple."""
    r, g, b = colour >> 11, (colour >> 5) & 0x3F, colour & 0x1F
    return (g >> 1) << 11 | ((b << 1) | (b >> 4)) << 5 | r


def turn_colours(dds):
    """A BC2 texture (a DDS, as the studio exports it) with its colours turned. Each 16-byte block is the alpha of its
    4x4 texels, then two RGB565 colours and a 2-bit index a texel that picks one of them or a blend of the two, so
    turning the two colours turns every texel exactly. Where the turn puts the smaller colour first, the two swap
    places (and the indices with them), so each block keeps the order its encoder gave it."""
    d = bytearray(dds)
    dx10 = d[84:88] == b"DX10"
    if d[:4] != b"DDS " or not (dx10 and struct.unpack_from("<I", d, 128)[0] in (74, 75) or d[84:88] == b"DXT3"):
        raise SystemExit("make_romfs: the exported icon isn't a BC2 DDS")
    for at in range(148 if dx10 else 128, len(d), 16):
        c0, c1, indices = struct.unpack_from("<HHI", d, at + 8)
        t0, t1 = turn(c0), turn(c1)
        if c0 > c1 and t0 < t1:
            t0, t1, indices = t1, t0, indices ^ 0x55555555
        struct.pack_into("<HHI", d, at + 8, t0, t1, indices)
    return bytes(d)


def edit_layout(files):
    layout = bflyt.Layout(sarc.find(files, LAYOUT))
    for name, data in files:
        if name.endswith(".bflan"):
            for target, _, tag, keys in bflyt.animated(data):
                if target == BAG_PANE and tag == "FLPA" and {"y", "scale_x", "scale_y"} & set(keys):
                    raise SystemExit("make_romfs: %s animates %s's height or scale" % (name, BAG_PANE))
    pane = layout.pane(BAG_PANE)
    pane.set(y=pane.get("y") + RAISE, scale=(BIGGER, BIGGER))
    return bytes(layout.data)


def edit_textures(tool, bntx):
    with tempfile.TemporaryDirectory() as scratch:
        current = os.path.join(scratch, "textures.bntx")
        open(current, "wb").write(bntx)
        exported = studio.run(tool, "export", "--file", current, "--output", os.path.join(scratch, "dds"), "--as", "dds")
        files = {w["texture"]: w["file"] for w in exported["written"]}
        for i, name in enumerate(BAG_ICONS):
            turned = os.path.join(scratch, "turned%d.dds" % i)
            open(turned, "wb").write(turn_colours(open(files[name], "rb").read()))
            out = os.path.join(scratch, "textures%d.bntx" % i)
            studio.run(tool, "import", "--file", current, "--texture", name, "--image", turned, "--output", out)
            current = out
        return open(current, "rb").read()


def edit_text():
    text, found = re.subn(r"^%s(@\d+)?\t.*$" % BAG_LABEL, lambda m: "%s%s\t%s" % (BAG_LABEL, m.group(1) or "", BAG_TEXT),
                          msg.dump(os.path.join(ROOT, "orig", "romfs", *MESSAGES.split("/"))), flags=re.M)
    if found != 1:
        raise SystemExit("make_romfs: %s has no line %s" % (MESSAGES, BAG_LABEL))
    dat, tbl = msg.build(text)
    write(MESSAGES, dat)
    table = MESSAGES[:-4] + ".tbl"
    if tbl != orig(table):   # the labels stay the same, so the game's own table does
        write(table, tbl)


def main():
    tool = studio.find(studio.given(sys.argv[1:]))
    files = sarc.read(orig(ARC))
    files = sarc.replace(files, LAYOUT, edit_layout(files))
    files = sarc.replace(files, TEXTURES, edit_textures(tool, sarc.find(files, TEXTURES)))
    write(ARC, sarc.write(files))
    edit_text()


if __name__ == "__main__":
    main()
