"""SARC archives (.arc), which hold the menus' layouts, animations and textures (docs/PLAN.md, roadmap phase 8).

Layout (little endian), as the game's tool wrote every .arc in Let's Go Pikachu 1.0.2, so an unchanged archive is
rebuilt byte for byte:
  header (0x14): "SARC", u16 0x14, u16 0xFEFF, u32 file size, u32 data offset, u16 0x100, u16 0
  SFAT: "SFAT", u16 0x0C, u16 file count, u32 hash key (0x65), then per file sorted by hash
        {u32 name hash, u32 0x01000000 | name offset / 4, u32 start, u32 end} (start and end from the data offset)
  SFNT: "SFNT", u16 8, u16 0, then the names, null-terminated and padded to 4 bytes, in node order
  the files in node order. Each starts at the next multiple of its alignment: for an NX binary file (byte-order mark
  FF FE at 0x0C, as BNTX textures have) 1 << its byte at 0x0E, at least 16; for any other file 16. The files start
  at the largest alignment any of them has; the archive ends where the last file does; padding is zero.
A name's hash: for each byte of its UTF-8, hash * 0x65 + the byte as a signed value, in 32 bits.

  import sarc
  files = sarc.read(data)                  # [(name, bytes)], in the archive's order
  files = sarc.replace(files, name, new)   # replaced, or added in hash order
  data = sarc.write(files)

  python -P tools/sarc.py list <file.arc>
  python -P tools/sarc.py extract <file.arc> <outdir>
  python -P tools/sarc.py replace <file.arc> <name> <file> <out.arc>   (adds the file when the archive hasn't got it)
  python -P tools/sarc.py roundtrip [files...]      read and write each archive (default: every .arc in orig/romfs)
                                                    and check the bytes come back the same
"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY = 0x65
HEADER, TABLE, NODE, NAMES = 0x14, 0x0C, 16, 8
NAMED = 0x01000000


class SarcError(ValueError):
    pass


def hash_name(name):
    h = 0
    for b in name.encode("utf-8"):
        h = (h * KEY + (b - 256 if b >= 128 else b)) & 0xFFFFFFFF
    return h


def alignment_of(data):
    if len(data) >= 0x10 and data[0x0C:0x0E] == b"\xFF\xFE" and data[0x0E] < 20:
        return max(16, 1 << data[0x0E])
    return 16


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def read(data):
    if data[:4] != b"SARC" or len(data) < HEADER + TABLE:
        raise SarcError("not a SARC archive")
    hsize, bom, size, data_off, version = struct.unpack_from("<HHIIH", data, 4)
    if hsize != HEADER or bom != 0xFEFF or version != 0x100:
        raise SarcError("not a little-endian SARC of version 0x100")
    if size > len(data) or data_off > size:
        raise SarcError("the archive is cut short")
    if data[HEADER:HEADER + 4] != b"SFAT":
        raise SarcError("no SFAT file table")
    count, key = struct.unpack_from("<HI", data, HEADER + 6)
    if key != KEY:
        raise SarcError("names hashed with %#x, not %#x" % (key, KEY))
    names_at = HEADER + TABLE + count * NODE
    if data[names_at:names_at + 4] != b"SFNT":
        raise SarcError("no SFNT name table")
    files = []
    for i in range(count):
        h, attr, start, end = struct.unpack_from("<IIII", data, HEADER + TABLE + i * NODE)
        if attr & 0xFF000000 != NAMED:
            raise SarcError("file %d has no name" % i)
        if start > end or data_off + end > size:
            raise SarcError("file %d runs past the end" % i)
        at = names_at + NAMES + (attr & 0xFFFFFF) * 4
        stop = data.find(b"\0", at, data_off)
        if stop < 0:
            raise SarcError("file %d's name runs past the name table" % i)
        name = data[at:stop].decode("utf-8")
        if hash_name(name) != h:
            raise SarcError("%s isn't under its own hash" % name)
        files.append((name, bytes(data[data_off + start:data_off + end])))
    return files


def write(files):
    files = sorted(files, key=lambda f: hash_name(f[0]))
    hashes = [hash_name(n) for n, _ in files]
    if len(set(hashes)) != len(hashes):
        raise SarcError("two names have the same hash")
    names = [n.encode("utf-8") for n, _ in files]
    name_offsets, at = [], 0
    for n in names:
        name_offsets.append(at)
        at += align(len(n) + 1, 4)
    names_at = HEADER + TABLE + len(files) * NODE
    alignments = [alignment_of(d) for _, d in files]
    data_off = align(names_at + NAMES + at, max(alignments + [16]))
    starts, at = [], data_off
    for (_, d), a in zip(files, alignments):
        starts.append(align(at, a))
        at = starts[-1] + len(d)
    out = bytearray(at)
    struct.pack_into("<4sHHIIHH", out, 0, b"SARC", HEADER, 0xFEFF, at, data_off, 0x100, 0)
    struct.pack_into("<4sHHI", out, HEADER, b"SFAT", TABLE, len(files), KEY)
    struct.pack_into("<4sHH", out, names_at, b"SFNT", NAMES, 0)
    for i, ((_, d), n) in enumerate(zip(files, names)):
        struct.pack_into("<IIII", out, HEADER + TABLE + i * NODE, hashes[i], NAMED | name_offsets[i] // 4,
                         starts[i] - data_off, starts[i] - data_off + len(d))
        p = names_at + NAMES + name_offsets[i]
        out[p:p + len(n)] = n
        out[starts[i]:starts[i] + len(d)] = d
    return bytes(out)


def replace(files, name, data):
    h = hash_name(name)
    for other, _ in files:
        if other != name and hash_name(other) == h:
            raise SarcError("%s has the same hash as %s" % (name, other))
    return sorted([f for f in files if f[0] != name] + [(name, bytes(data))], key=lambda f: hash_name(f[0]))


def find(files, name):
    for n, d in files:
        if n == name:
            return d
    raise SarcError("the archive has no %s" % name)


def roundtrip(paths):
    bad = 0
    for path in paths:
        data = open(path, "rb").read()
        try:
            same = write(read(data)) == data
        except SarcError as e:
            print("FAIL %s: %s" % (path, e))
            bad += 1
            continue
        if not same:
            print("FAIL %s: rebuilt differently" % path)
            bad += 1
    print("%d of %d archives rebuilt byte for byte" % (len(paths) - bad, len(paths)))
    return bad == 0


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "list":
            for name, d in read(open(argv[2], "rb").read()):
                print("%08x %9d %s" % (hash_name(name), len(d), name))
        elif len(argv) == 4 and argv[1] == "extract":
            for name, d in read(open(argv[2], "rb").read()):
                path = os.path.join(argv[3], *name.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                open(path, "wb").write(d)
        elif len(argv) == 6 and argv[1] == "replace":
            files = replace(read(open(argv[2], "rb").read()), argv[3], open(argv[4], "rb").read())
            open(argv[5], "wb").write(write(files))
        elif len(argv) >= 2 and argv[1] == "roundtrip":
            paths = argv[2:] or sorted(glob.glob(os.path.join(ROOT, "orig", "romfs", "**", "*.arc"), recursive=True))
            return 0 if roundtrip(paths) else 1
        else:
            print(__doc__)
            return 2
    except (SarcError, OSError) as e:
        print("sarc: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
