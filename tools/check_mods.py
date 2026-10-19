"""The runtime's mods folder (runtime/src/mods.cpp) checked outside the game, through tools/modstest.

  python -P tools/check_mods.py [--real N | --real all] [--archive bin/archive/....gfpak]... [--names FILE]

First on a made-up archive: two mods replace one file (the later mod's wins), add files to a folder
and to the last folder, and leave the rest alone. Then on some of the game's own archives
(orig/romfs): three whose files the game's strings mostly name, N picked the same way every time,
or all of them. Every file in one that can be named from strings in main and in the archive (and
in --names, a list of the archives' paths such as letsgostudio's GfpakNames finds: one per line,
the last tab-separated column) is put back in through a mod as it was, so the runtime compresses
it again, and a new file is added to one of its folders. Every entry of the archive modstest
writes must hold what it should. Files made from the game stay in build/mods_check/.

Needs build/pc/modstest.exe (cmake --build build/pc --target modstest; LETSGO_BUILD picks another
build) and Python's lz4.
"""
import argparse
import glob
import os
import random
import re
import shutil
import struct
import subprocess
import sys

import lz4.block

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODSTEST = os.path.join(os.environ.get("LETSGO_BUILD", os.path.join(ROOT, "build", "pc")), "modstest.exe")
WORK = os.path.join(ROOT, "build", "mods_check")
ROMFS = os.path.join(ROOT, "orig", "romfs")
DEFAULT = ["bin/archive/field/placement.gfpak", "bin/archive/chara/data/ob/ob0204_00_monsterball.gfpak",
           "bin/archive/capture/capture_game.gfpak"]
MASK = (1 << 64) - 1


def fnv(b):
    h = 0xCBF29CE484222645
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & MASK
    return h


def parse(data):
    """entries: dicts in index order (path, name, level, comp, size, raw, offset); folders: (hash, [index])"""
    if data[:8] != b"GFLXPACK":
        raise ValueError("not a gfpak")
    _ver, _rel, n, nf, rec, hashes = struct.unpack_from("<IIIIQQ", data, 8)
    entries = []
    for i in range(n):
        level, comp, size, stored, _u, off = struct.unpack_from("<HHIIIQ", data, rec + 24 * i)
        path, = struct.unpack_from("<Q", data, hashes + 8 * i)
        entries.append({"path": path, "name": None, "level": level, "comp": comp, "size": size,
                        "raw": data[off:off + stored], "offset": off, "unused": _u})
    folders = []
    for f in range(nf):
        at, = struct.unpack_from("<Q", data, 0x28 + 8 * f)
        h, count, u = struct.unpack_from("<QII", data, at)
        assert u == 0xCC, "folder %d: unused field %x" % (f, u)
        idx = []
        for i in range(count):
            name, index, u = struct.unpack_from("<QII", data, at + 16 + 16 * i)
            assert u == 0xCC and entries[index]["name"] is None, "folder %d entry %d" % (f, i)
            entries[index]["name"] = name
            idx.append(index)
        folders.append((h, idx))
    assert all(e["name"] is not None for e in entries), "an entry in no folder"
    return entries, folders


def write(files, folders):
    """A made-up archive in the game's layout: files [(path, name, level, raw, size)], folders (hash, [index])."""
    n, nf = len(files), len(folders)
    hashes = 0x28 + 8 * nf
    at = hashes + 8 * n
    fat = []
    for _h, idx in folders:
        fat.append(at)
        at += 16 + 16 * len(idx)
    rec = at
    at += 24 * n
    offs = []
    for f in files:
        at = (at + 15) & ~15
        offs.append(at)
        at += len(f[3])
    out = bytearray((at + 15) & ~15)
    out[0:8] = b"GFLXPACK"
    struct.pack_into("<IIIIQQ", out, 8, 0x1000, 0, n, nf, rec, hashes)
    for i, f in enumerate(files):
        struct.pack_into("<Q", out, hashes + 8 * i, f[0])
        struct.pack_into("<HHIIIQ", out, rec + 24 * i, f[2], 2, f[4], len(f[3]), 0xCC, offs[i])
        out[offs[i]:offs[i] + len(f[3])] = f[3]
    for k, (h, idx) in enumerate(folders):
        struct.pack_into("<Q", out, 0x28 + 8 * k, fat[k])
        struct.pack_into("<QII", out, fat[k], h, len(idx), 0xCC)
        for j, i in enumerate(idx):
            struct.pack_into("<QII", out, fat[k] + 16 + 16 * j, files[i][1], i, 0xCC)
    return bytes(out)


def content(e):
    return lz4.block.decompress(e["raw"], uncompressed_size=e["size"]) if e["size"] else b""


def put(mods, mod, rel, data):
    p = os.path.join(mods, mod, "romfs", *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)


def run(mods, archive, rel, out):
    r = subprocess.run([MODSTEST, mods, archive, rel, out], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("modstest failed:\n" + r.stdout + r.stderr)
    return r.stdout + r.stderr


def check(original, patched, replaced, added):
    """replaced: path hash -> new content; added: (folder hash, name hash, path hash, content)."""
    old, old_folders = parse(original)
    new, new_folders = parse(patched)
    assert len(patched) % 16 == 0, "size not padded to 16"
    assert len(new) == len(old) + len(added), "%d entries, expected %d" % (len(new), len(old) + len(added))
    assert [h for h, _ in new_folders] == [h for h, _ in old_folders], "folders changed"
    folder_of = {}
    for h, idx in new_folders:
        for i in idx:
            folder_of[i] = h
        if idx:
            assert sorted(idx) == list(range(min(idx), min(idx) + len(idx))), "a folder's files aren't together"
    by_path = {e["path"]: (i, e) for i, e in enumerate(new)}
    assert len(by_path) == len(new), "a path hash twice"
    old_folder_of = {i: h for h, idx in old_folders for i in idx}
    for i, e in enumerate(old):
        j, f = by_path[e["path"]]
        assert f["name"] == e["name"] and folder_of[j] == old_folder_of[i], "entry %016x moved folder" % e["path"]
        assert f["offset"] % 16 == e["offset"] % 16, "entry %016x alignment" % e["path"]
        if e["path"] in replaced:
            assert f["comp"] == 2 and f["level"] == e["level"], "replaced %016x header" % e["path"]
            assert content(f) == replaced[e["path"]], "replaced %016x content" % e["path"]
        else:
            assert (f["raw"], f["comp"], f["size"], f["level"]) == (e["raw"], e["comp"], e["size"], e["level"]), \
                "unchanged %016x differs" % e["path"]
    for fh, nh, ph, data in added:
        j, f = by_path[ph]
        assert folder_of[j] == fh and f["name"] == nh and f["level"] == 9 and f["comp"] == 2, "added %016x" % ph
        assert content(f) == data, "added %016x content" % ph
    assert all(e["unused"] == 0xCC for e in new), "a record's unused field"


def made_up():
    rnd = random.Random(1)
    work = os.path.join(WORK, "made_up")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    folder_names = ["bin/test/a/", "bin/test/b/", "bin/test/c/"]
    files, folders, data = [], [], {}
    for fname in folder_names:
        idx = []
        for k in range(4):
            name = "file%d.bin" % k
            if k % 2:
                d = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 3000)))
            else:
                d = bytes(rnd.choice(b"abcab") for _ in range(rnd.randrange(0, 20000)))
            raw = lz4.block.compress(d, store_size=False)
            idx.append(len(files))
            files.append((fnv((fname + name).encode()), fnv(name.encode()), 9, raw, len(d)))
            data[fname + name] = d
        folders.append((fnv(fname.encode()), idx))
    archive = os.path.join(work, "test.gfpak")
    open(archive, "wb").write(write(files, folders))
    mods = os.path.join(work, "mods")
    samples = [b"", b"x", b"0123456789ab", bytes(100000), bytes(rnd.randrange(256) for _ in range(70000)),
               b"abc" * 30000 + bytes(rnd.randrange(256) for _ in range(17))]
    replaced, added = {}, []
    put(mods, "a_first", "bin/test/b/file1.bin", b"overridden by b_second")
    put(mods, "b_second", "bin/test/b/file1.bin", samples[5])
    replaced[fnv(b"bin/test/b/file1.bin")] = samples[5]
    put(mods, "a_first", "bin/test/a/file2.bin", samples[3])
    replaced[fnv(b"bin/test/a/file2.bin")] = samples[3]
    for k, (fname, d) in enumerate([("bin/test/a/", samples[0]), ("bin/test/a/", samples[4]), ("bin/test/c/", samples[1]),
                                    ("bin/test/c/", samples[2])]):
        name = "new%d.bin" % k
        put(mods, "a_first", fname + name, d)
        added.append((fnv(fname.encode()), fnv(name.encode()), fnv((fname + name).encode()), d))
    put(mods, "a_first", "bin/elsewhere/other.bin", b"in no folder of this archive")
    out = os.path.join(work, "out.gfpak")
    run(mods, archive, "bin/archive/test.gfpak", out)
    check(open(archive, "rb").read(), open(out, "rb").read(), replaced, added)
    print("made-up archive: ok (2 replaced, 4 added, 1 left out)")


def strings(data):
    return set(m.group() for m in re.finditer(rb"[\x21-\x7e]{3,200}", data))


def name_tables(strs):
    """hash -> name for every string and its path parts, and hash -> folder ("a/b/") for every string
    as a folder and every path's folder"""
    names, folders = {}, {}
    for s in strs:
        for part in [s] + s.split(b"/"):
            if part:
                names.setdefault(fnv(part), part)
        f = s if s.endswith(b"/") else s + b"/"
        folders.setdefault(fnv(f), f)
        if b"/" in s:
            f = s[:s.rfind(b"/") + 1]
            folders.setdefault(fnv(f), f)
    return names, folders


def real(rels, exe_names, exe_folders):
    total = 0
    for rel in rels:
        archive = os.path.join(ROMFS, *rel.split("/"))
        original = open(archive, "rb").read()
        entries, folders = parse(original)
        local = set()
        for e in entries:
            if e["comp"] == 2:
                local |= strings(content(e))
        local_names, local_folders = name_tables(local)
        names = {**exe_names, **local_names}
        folder_names = {**exe_folders, **local_folders}
        work = os.path.join(WORK, "real")
        shutil.rmtree(work, ignore_errors=True)
        mods = os.path.join(work, "mods")
        replaced, added = {}, []
        for fh, idx in folders:
            folder = folder_names.get(fh)
            if folder is None:
                continue
            for i in idx:
                e = entries[i]
                name = names.get(e["name"])
                if name is None or fnv(folder + name) != e["path"]:
                    continue
                d = content(e)
                put(mods, "identity", (folder + name).decode("ascii", "replace"), d)
                replaced[e["path"]] = d
            if not added:
                name = b"check_mods_added.bin"
                d = b"added by tools/check_mods.py\n" * 50
                put(mods, "identity", (folder + name).decode("ascii", "replace"), d)
                added.append((fh, fnv(name), fnv(folder + name), d))
        if not replaced:
            print("%s: no file named, skipped" % rel)
            continue
        out = os.path.join(work, "out.gfpak")
        run(mods, archive, rel, out)
        check(original, open(out, "rb").read(), replaced, added)
        total += len(replaced)
        print("%s: ok (%d of %d files put back, %d added)" % (rel, len(replaced), len(entries), len(added)))
        shutil.rmtree(work, ignore_errors=True)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", help="how many of the game's archives, or 'all' (default: three the game names)")
    ap.add_argument("--archive", action="append", default=[], help="a RomFS path to check (bin/archive/...)")
    ap.add_argument("--names", help="more names for the archives' files (one per line, the last tab-separated column)")
    a = ap.parse_args()
    if not os.path.exists(MODSTEST):
        raise SystemExit("build tools/modstest first: cmake --build build/pc --target modstest")
    made_up()
    rels = a.archive or (DEFAULT if a.real is None else [])
    if not rels:
        every = sorted(os.path.relpath(p, ROMFS).replace("\\", "/")
                       for p in glob.glob(os.path.join(ROMFS, "bin", "**", "*.gfpak"), recursive=True))
        rels = every if a.real == "all" else random.Random(0).sample(every, min(int(a.real), len(every)))
    if rels:
        known = strings(open(os.path.join(ROOT, "orig", "elf", "main.elf"), "rb").read())
        if a.names:
            known |= {line.rstrip(b"\r\n").split(b"\t")[-1] for line in open(a.names, "rb") if line.strip()}
        n = real(rels, *name_tables(known))
        print("%d of the game's files put back through mods in %d archives" % (n, len(rels)))
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
