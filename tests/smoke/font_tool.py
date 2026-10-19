"""tools/font.py on made-up fonts: encoded as the game keeps its fonts and decoded back, a size that isn't a multiple of 4,
the header's words, and the mistakes it refuses.

  python -P tests/smoke/font_tool.py      (prints PASS or what failed)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import font  # noqa: E402


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def refuses(what, f, *args):
    try:
        f(*args)
    except font.FontError:
        return
    print("FAIL accepted " + what)
    sys.exit(1)


def main():
    for made_up in (b"OTTO" + bytes(range(200)), b"\0\1\0\0" + b"glyphs" * 7 + b"x"):   # 204 bytes, and 47
        data = font.encode(made_up)
        check(len(data) == 8 + len(made_up) + (-len(made_up) % 4), "padded to a multiple of 4")
        magic, size = struct.unpack_from("<II", data)
        check(magic ^ font.KEY == font.MAGIC, "the magic, XORed")
        check(int.from_bytes((size ^ font.KEY).to_bytes(4, "little"), "big") == len(made_up), "the size, big-endian and XORed")
        check(struct.unpack_from("<I", data, 8)[0] == struct.unpack_from("<I", made_up)[0] ^ font.KEY, "the font's first word, XORed")
        check(font.decode(data) == made_up, "decoded back")
        check(font.encode(font.decode(data)) == data, "encoded again the same")
    refuses("a file that isn't a font", font.encode, b"PNG and more")
    refuses("a game font that isn't one", font.decode, b"\0" * 16)
    refuses("a size past the end", font.decode, font.encode(b"OTTO" + bytes(20))[:16])
    refuses("too short", font.decode, b"abc")
    print("PASS")


if __name__ == "__main__":
    main()
