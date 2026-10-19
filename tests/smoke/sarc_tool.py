"""tools/sarc.py on a made-up archive: files laid out in hash order with their alignments (an NX binary file on its own),
written and read back the same, a file replaced and one added, and the mistakes it refuses.

  python -P tests/smoke/sarc_tool.py      (prints PASS or what failed)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import sarc  # noqa: E402


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def refuses(what, f, *args):
    try:
        f(*args)
    except sarc.SarcError:
        return
    print("FAIL accepted " + what)
    sys.exit(1)


def main():
    # A made-up NX binary file: the byte-order mark FF FE at 0x0C and 12 at 0x0E, so it starts on a multiple of 4096.
    nx = b"BNTX\0\0\0\0\0\0\4\0\xff\xfe\x0c\0" + bytes(range(40))
    files = [("blyt/screen_00.bflyt", b"FLYT" + bytes(29)), ("timg/__Combined.bntx", nx), ("anim/screen_00_in.bflan", b"FLAN" + bytes(7))]
    check(sarc.hash_name("a") == 0x61 and sarc.hash_name("ab") == (0x61 * 0x65 + 0x62), "name hashes")
    check(sarc.hash_name("\xe9") == ((0xC3 - 256) * 0x65 + (0xA9 - 256)) & 0xFFFFFFFF, "a UTF-8 name's bytes count as signed")
    check(sarc.alignment_of(nx) == 4096 and sarc.alignment_of(files[0][1]) == 16, "alignments")

    data = sarc.write(files)
    back = sarc.read(data)
    check([n for n, _ in back] == sorted([n for n, _ in files], key=sarc.hash_name), "files in hash order")
    check(dict(back) == dict(files), "the files come back")
    check(sarc.write(back) == data, "written again the same")
    check(struct.unpack_from("<I", data, 8)[0] == len(data), "the size in the header")
    data_off = struct.unpack_from("<I", data, 12)[0]
    check(data_off % 4096 == 0, "the files start at the largest alignment")
    starts = {}
    for i in range(3):
        h, attr, start, end = struct.unpack_from("<IIII", data, 0x20 + 16 * i)
        starts[h] = data_off + start
    check(starts[sarc.hash_name("timg/__Combined.bntx")] % 4096 == 0, "the NX file on a multiple of 4096")
    check(all(s % 16 == 0 for s in starts.values()), "every file on a multiple of 16")
    check(data.endswith(back[-1][1]) and max(starts.values()) == len(data) - len(back[-1][1]), "the archive ends with its last file")

    bigger = sarc.replace(back, "blyt/screen_00.bflyt", b"FLYT" + bytes(100))
    check(len(sarc.find(bigger, "blyt/screen_00.bflyt")) == 104 and sarc.find(bigger, "timg/__Combined.bntx") == nx, "replaced one file")
    added = sarc.replace(bigger, "blyt/new_00.bflyt", b"FLYT new")
    check([n for n, _ in added] == sorted([n for n, _ in added], key=sarc.hash_name) and len(added) == 4, "added in hash order")
    check(dict(sarc.read(sarc.write(added))) == dict(added), "the changed archive reads back")

    # aBz and aC\x15 hash the same: 0x42 * 0x65 + 0x7A == 0x43 * 0x65 + 0x15
    refuses("two names with one hash", sarc.replace, [("aBz", b"1")], "aC\x15", b"2")
    refuses("not a SARC", sarc.read, b"SARD" + data[4:])
    refuses("another hash key", sarc.read, data[:0x1C] + struct.pack("<I", 0x66) + data[0x20:])
    refuses("a name under another's hash", sarc.read, data[:0x20] + struct.pack("<I", 1) + data[0x24:])
    refuses("a missing file", sarc.find, back, "blyt/none.bflyt")
    refuses("a cut-short archive", sarc.read, data[:data_off])
    print("PASS")


if __name__ == "__main__":
    main()
