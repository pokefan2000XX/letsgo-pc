"""The game's fonts (bin/font/*.BFOTF) to OpenType files and back (docs/MENUS.md, roadmap phase 8).

A .BFOTF is a font file XORed 32 bits at a time with 0x06186249 (the key of the system's .bfttf fonts,
tools/extract_fonts.py) after an 8-byte header: u32 0x18029A7F and the font's size as a big-endian u32, both XORed the
same way. The font is padded with zeros to a multiple of 4 before the XOR. Let's Go's are OpenType (CFF) fonts:
FOT-UDKakugoC80Pro-DB, which draws the English text of the menus and message boxes, FOT-RodinNTLGPro-DB and the small
beluga_font. The game also takes TrueType fonts (hacks/new-font). num_00.bffnt is a bitmap font, not this.

  python -P tools/font.py decode <file.BFOTF> <out.otf>
  python -P tools/font.py encode <file.otf | file.ttf> <out.BFOTF>
  python -P tools/font.py roundtrip [files...]      decode and encode each (default: bin/font/*.BFOTF in orig/romfs)
                                                    and check the bytes come back the same
"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY = 0x06186249
MAGIC = 0x18029A7F


class FontError(ValueError):
    pass


def xor(data):
    padded = bytes(data) + bytes(-len(data) % 4)
    n = len(padded) // 4
    return struct.pack("<%dI" % n, *(w ^ KEY for w in struct.unpack("<%dI" % n, padded)))


def decode(data):
    if len(data) < 8:
        raise FontError("too short for a font")
    magic, size = struct.unpack_from("<II", xor(data[:8]))
    size = int.from_bytes(size.to_bytes(4, "little"), "big")
    if magic != MAGIC:
        raise FontError("not a game font (its first word is %#010x)" % (struct.unpack_from("<I", data)[0]))
    if size > len(data) - 8:
        raise FontError("it says %d bytes but has %d" % (size, len(data) - 8))
    return xor(data[8:8 + size + (-size % 4)])[:size]


def encode(font):
    if font[:4] not in (b"OTTO", b"\0\1\0\0", b"true"):
        raise FontError("not an OpenType or TrueType font")
    return xor(struct.pack("<I", MAGIC) + len(font).to_bytes(4, "big") + font)


def roundtrip(paths):
    bad = 0
    for path in paths:
        data = open(path, "rb").read()
        try:
            font = decode(data)
            same = encode(font) == data
        except FontError as e:
            print("FAIL %s: %s" % (path, e))
            bad += 1
            continue
        if not same:
            print("FAIL %s: encoded differently" % path)
            bad += 1
        else:
            print("%s: %s, %d bytes" % (os.path.basename(path), "OpenType (CFF)" if font[:4] == b"OTTO" else "TrueType", len(font)))
    print("%d of %d fonts came back byte for byte" % (len(paths) - bad, len(paths)))
    return bad == 0


def main(argv):
    try:
        if len(argv) == 4 and argv[1] == "decode":
            open(argv[3], "wb").write(decode(open(argv[2], "rb").read()))
        elif len(argv) == 4 and argv[1] == "encode":
            open(argv[3], "wb").write(encode(open(argv[2], "rb").read()))
        elif len(argv) >= 2 and argv[1] == "roundtrip":
            paths = argv[2:] or sorted(glob.glob(os.path.join(ROOT, "orig", "romfs", "bin", "font", "*.BFOTF")))
            return 0 if paths and roundtrip(paths) else 1
        else:
            print(__doc__)
            return 2
    except (FontError, OSError) as e:
        print("font: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
