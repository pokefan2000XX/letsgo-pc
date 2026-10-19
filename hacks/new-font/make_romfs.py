"""Makes the new-font hack's files for the game (roadmap phase 8, fonts): one of the game's fonts replaced with an OpenType or
TrueType font you give it, written as the game keeps its fonts (tools/font.py). By default it replaces
FOT-UDKakugoC80Pro-DB, which draws the English text of the menus and the message boxes; --as picks another of bin/font/
(replacing FOT-RodinNTLGPro-DB changed nothing in the main menu).

A font is someone's work: use one whose licence lets you, and it stays in hacks/new-font/romfs/ on your PC (romfs/ is
ignored by git). The font needs the characters the game writes (é, ♂ and ♀ among them); a character it lacks shows as
nothing or a box.

  python -P hacks/new-font/make_romfs.py --font <file.otf | file.ttf> [--as <name>]
  build/pc/letsgo.exe --mods hacks/new-font
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import font  # noqa: E402

FONTS = ("FOT-UDKakugoC80Pro-DB", "FOT-RodinNTLGPro-DB", "beluga_font")


def option(args, name, default=None):
    if name in args and args.index(name) + 1 < len(args):
        return args[args.index(name) + 1]
    return default


def main():
    args = sys.argv[1:]
    given, name = option(args, "--font"), option(args, "--as", FONTS[0])
    if not given:
        raise SystemExit(__doc__)
    if name not in FONTS:
        raise SystemExit("make_romfs: --as is one of %s" % ", ".join(FONTS))
    try:
        data = font.encode(open(given, "rb").read())
    except (font.FontError, OSError) as e:
        raise SystemExit("make_romfs: %s: %s" % (given, e))
    out = os.path.join(HERE, "romfs", "bin", "font", name + ".BFOTF")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data)
    print("wrote", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
