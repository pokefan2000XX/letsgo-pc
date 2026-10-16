"""GameFreak GFLXPACK (.gfpak) reader.

Layout (little endian):
  header: "GFLXPACK", u32 version (0x1000), u32 relocated, u32 file count,
          u32 folder count, u64 file-info offset, u64 path-hash table offset,
          u64 folder offsets[folder count]
  folder: u64 path hash, u32 file count, u32 pad, then per file {u64 name hash, u32 index, u32 pad}
  file info (24 bytes each): u16 level, u16 compression (0 none, 1 zlib, 2 LZ4 block),
          u32 decompressed size, u32 compressed size, u32 pad, u64 data offset

Usage as a tool:  python -P tools/gfpak.py <file.gfpak> [outdir]
  lists the entries (hash, sizes) and, with outdir, writes each decompressed entry
  as <index>_<pathhash>.bin (the pack stores hashes, not names).
"""
import os
import struct
import sys
import zlib


def read(path):
    b = open(path, "rb").read()
    if b[:8] != b"GFLXPACK":
        raise ValueError("%s: not a GFLXPACK" % path)
    _ver, _rel, nfiles, nfolders, info_off, hash_off = struct.unpack_from("<IIIIQQ", b, 8)
    path_hashes = struct.unpack_from("<%dQ" % nfiles, b, hash_off) if hash_off else [0] * nfiles
    entries = []
    for i in range(nfiles):
        _lvl, comp, dsize, csize, _pad, off = struct.unpack_from("<HHIIIQ", b, info_off + 24 * i)
        raw = b[off:off + csize]
        entries.append({"index": i, "hash": path_hashes[i], "comp": comp, "size": dsize, "csize": csize, "raw": raw})
    return entries


def data(entry):
    c = entry["comp"]
    if c == 0:
        return entry["raw"]
    if c == 1:
        return zlib.decompress(entry["raw"])
    if c == 2:
        import lz4.block
        return lz4.block.decompress(entry["raw"], uncompressed_size=entry["size"])
    raise ValueError("unknown compression %d" % c)


def main():
    entries = read(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if out:
        os.makedirs(out, exist_ok=True)
    for e in entries:
        d = data(e)
        print("%3d %016x comp=%d %9d %s" % (e["index"], e["hash"], e["comp"], len(d), d[:8]))
        if out:
            open(os.path.join(out, "%03d_%016x.bin" % (e["index"], e["hash"])), "wb").write(d)


if __name__ == "__main__":
    main()
