#!/usr/bin/env python3
"""Extract Let's Go Pikachu v1.0.2 from the user's own dump.

Reads the base NSP and the update NSP, decrypts the NCAs with the user's
prod.keys, and writes:
  <out>/exefs/   raw NSOs (main, sdk, subsdk0, subsdk1, rtld) + main.npdm from the update
  <out>/romfs/   the RomFS with the update's BKTR patch applied
  <out>/manifest.json

Every RomFS block is checked against the IVFC SHA-256 hash tree, from the
master hash in the NCA header down to the file data, so a bad key, a bad dump
or a BKTR bug shows up as a hash failure instead of a corrupt file.

Usage:
  python -P tools/nxextract.py --base BASE.nsp --update UPDATE.nsp --keys prod.keys --out orig
"""
import argparse
import bisect
import hashlib
import json
import os
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

CONTENT_TYPES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}
ENC_NONE, ENC_XTS, ENC_CTR, ENC_BKTR = 1, 2, 3, 4


# ---------------------------------------------------------------- crypto ---

def load_keys(path):
    keys = {}
    for line in open(path, encoding="utf-8", errors="ignore"):
        if "=" in line:
            k, v = line.split("=", 1)
            try:
                keys[k.strip()] = bytes.fromhex(v.strip())
            except ValueError:
                pass
    return keys


def aes_ecb_dec(key, data):
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


def aes_ctr(key, counter, data):
    d = Cipher(algorithms.AES(key), modes.CTR(counter)).decryptor()
    return d.update(data) + d.finalize()


def aes_xts_dec_nintendo(key, data, first_sector, sector_size=0x200):
    """NCA headers use AES-XTS with a big-endian sector number as the tweak."""
    out = bytearray()
    for i in range(0, len(data), sector_size):
        tweak = (first_sector + i // sector_size).to_bytes(16, "big")
        d = Cipher(algorithms.AES(key), modes.XTS(tweak)).decryptor()
        out += d.update(data[i:i + sector_size]) + d.finalize()
    return bytes(out)


# ------------------------------------------------------------ containers ---

class FileRange:
    """Random access into one region of a host file."""

    def __init__(self, f, offset, size):
        self.f, self.offset, self.size = f, offset, size

    def read(self, off, size):
        self.f.seek(self.offset + off)
        return self.f.read(size)


def parse_pfs0(read, base=0):
    """Returns [(name, offset, size)] with offsets relative to the reader."""
    hdr = read(base, 0x10)
    if hdr[:4] != b"PFS0":
        raise ValueError("not a PFS0: %r" % hdr[:4])
    count, strsz = struct.unpack_from("<II", hdr, 4)
    entries = read(base + 0x10, count * 0x18)
    strings = read(base + 0x10 + count * 0x18, strsz)
    data0 = base + 0x10 + count * 0x18 + strsz
    out = []
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", entries, i * 0x18)
        name = strings[name_off:strings.index(b"\0", name_off)].decode()
        out.append((name, data0 + off, size))
    return out


class Nsp:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.files = parse_pfs0(lambda o, n: self._read(o, n))
        self.title_keys = {}
        for name, off, size in self.files:
            if name.endswith(".tik"):
                tik = self._read(off, size)
                sig_type = struct.unpack_from("<I", tik, 0)[0]
                body = {0x10004: 0x140, 0x10003: 0x240, 0x10005: 0x80,
                        0x10001: 0x240, 0x10000: 0x240, 0x10002: 0x80}[sig_type]
                enc_title_key = tik[body + 0x40:body + 0x50]
                rights_id = tik[body + 0x160:body + 0x170]
                self.title_keys[rights_id] = enc_title_key

    def _read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def ncas(self):
        return [(n, o, s) for n, o, s in self.files if n.endswith(".nca")]


class Section:
    """One NCA filesystem section, readable as plaintext with AES-CTR."""

    def __init__(self, nca, index, start, size, fs_header):
        self.nca, self.index, self.start, self.size, self.fsh = nca, index, start, size, fs_header
        self.fs_type, self.hash_type, self.enc_type = fs_header[2], fs_header[3], fs_header[4]
        # Upper 8 bytes of the CTR: the u64 at 0x140, big-endian.
        self.ctr_hi = fs_header[0x140:0x148][::-1]

    def _counter(self, abs_nca_off, generation=None):
        hi = self.ctr_hi if generation is None else self.ctr_hi[:4] + generation.to_bytes(4, "big")
        return hi + (abs_nca_off >> 4).to_bytes(8, "big")

    def read_raw(self, off, size):
        return self.nca.raw.read(self.start + off, size)

    def read_ctr(self, off, size, generation=None):
        a = off & ~0xF
        b = (off + size + 0xF) & ~0xF
        data = self.read_raw(a, b - a)
        if self.enc_type != ENC_NONE:
            data = aes_ctr(self.nca.ctr_key, self._counter(self.start + a, generation), data)
        return data[off - a:off - a + size]

    read = read_ctr


class Nca:
    def __init__(self, nsp, name, offset, size, keys):
        self.name = name
        self.raw = FileRange(nsp.f, offset, size)
        hdr = aes_xts_dec_nintendo(keys["header_key"], self.raw.read(0, 0xC00), 0)
        if hdr[0x200:0x204] != b"NCA3":
            raise ValueError("%s: bad NCA magic %r (wrong header_key?)" % (name, hdr[0x200:0x204]))
        self.header = hdr
        self.content_type = CONTENT_TYPES.get(hdr[0x205], hdr[0x205])
        self.key_generation = max(hdr[0x206], hdr[0x220])
        master_key_rev = max(self.key_generation - 1, 0)
        self.title_id = struct.unpack_from("<Q", hdr, 0x210)[0]
        rights_id = hdr[0x230:0x240]
        if any(rights_id):
            enc = nsp.title_keys.get(rights_id)
            if enc is None:
                raise ValueError("%s: needs a title key for rights ID %s (no ticket in NSP)" % (name, rights_id.hex()))
            self.ctr_key = aes_ecb_dec(keys["titlekek_%02x" % master_key_rev], enc)
        else:
            area = ["application", "ocean", "system"][hdr[0x207]]
            kak = keys["key_area_key_%s_%02x" % (area, master_key_rev)]
            self.ctr_key = aes_ecb_dec(kak, hdr[0x300:0x340])[0x20:0x30]
        self.sections = {}
        for i in range(4):
            st, en = struct.unpack_from("<II", hdr, 0x240 + i * 0x10)
            if en:
                self.sections[i] = Section(self, i, st * 0x200, (en - st) * 0x200,
                                           hdr[0x400 + i * 0x200:0x600 + i * 0x200])


# ------------------------------------------------------------------ BKTR ---

def parse_bucket_tree(blob, entry_size, entry_fmt):
    """BKTR bucket tree: a 0x4000 L1 node, then 0x4000-byte entry sets."""
    _, set_count, end_offset = struct.unpack_from("<IIQ", blob, 0)
    if not (0 < set_count <= 0x7FE):
        raise ValueError("bad bucket tree set count %d" % set_count)
    entries = []
    for s in range(set_count):
        base = 0x4000 + s * 0x4000
        _, n, _end = struct.unpack_from("<IIQ", blob, base)
        if n > (0x4000 - 0x10) // entry_size:
            raise ValueError("bad bucket entry count %d" % n)
        for e in range(n):
            entries.append(struct.unpack_from(entry_fmt, blob, base + 0x10 + e * entry_size))
    return end_offset, entries


class AesCtrExReader:
    """Patch data of a BKTR section: each subsection carries its own CTR generation."""

    def __init__(self, section, subsections, end):
        self.s = section
        self.starts = [o for o, _, _ in subsections]
        self.gens = [g for _, _, g in subsections]
        self.end = end

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            i = bisect.bisect_right(self.starts, off) - 1
            nxt = self.starts[i + 1] if i + 1 < len(self.starts) else self.end
            n = min(size, nxt - off)
            out += self.s.read_ctr(off, n, self.gens[i])
            off += n
            size -= n
        return bytes(out)


class BktrReader:
    """The patched (virtual) RomFS section: ranges come from the base or the patch."""

    def __init__(self, patch_section, base_section):
        fsh = patch_section.fsh
        ind_off, ind_size = struct.unpack_from("<QQ", fsh, 0x100)
        ctr_off, ctr_size = struct.unpack_from("<QQ", fsh, 0x120)
        if fsh[0x110:0x114] != b"BKTR" or fsh[0x130:0x134] != b"BKTR":
            raise ValueError("update RomFS section has no BKTR headers")
        sub_end, subs = parse_bucket_tree(patch_section.read_ctr(ctr_off, ctr_size), 0x10, "<QII")
        self.patch = AesCtrExReader(patch_section, subs, ctr_off)
        try:
            ind_blob = self.patch.read(ind_off, ind_size)
            self.size, rel = parse_bucket_tree(ind_blob, 0x14, "<QQi")
        except ValueError:
            # Older layouts encrypt the indirect table with the section's own counter.
            ind_blob = patch_section.read_ctr(ind_off, ind_size)
            self.size, rel = parse_bucket_tree(ind_blob, 0x14, "<QQi")
        self.v_starts = [v for v, _, _ in rel]
        self.rel = rel
        self.base = base_section
        self.stats = {"indirect_entries": len(rel), "aesctrex_entries": len(subs),
                      "patched_bytes": 0, "virtual_size": self.size}
        for i, (v, p, idx) in enumerate(rel):
            nxt = self.v_starts[i + 1] if i + 1 < len(rel) else self.size
            if idx == 1:
                self.stats["patched_bytes"] += nxt - v

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            i = bisect.bisect_right(self.v_starts, off) - 1
            v, p, idx = self.rel[i]
            nxt = self.v_starts[i + 1] if i + 1 < len(self.rel) else self.size
            n = min(size, nxt - off)
            src = self.patch if idx == 1 else self.base
            out += src.read(p + (off - v), n)
            off += n
            size -= n
        return bytes(out)


# ------------------------------------------------------------ IVFC/RomFS ---

def ivfc_levels(fsh):
    ivfc = fsh[0x8:0x8 + 0xF8]
    if ivfc[:4] != b"IVFC":
        raise ValueError("no IVFC header")
    master_size = struct.unpack_from("<I", ivfc, 8)[0]
    levels = [struct.unpack_from("<QQI", ivfc, 0x10 + i * 0x18) for i in range(6)]
    master = ivfc[0xC0:0xC0 + master_size]
    return master, levels


def verify_small_levels(reader, master, levels):
    """Checks IVFC levels 1..5 (hash data) from the master hash down."""
    prev = master
    for i in range(5):
        off, size, bs_log2 = levels[i]
        data = reader.read(off, size)
        bs = 1 << bs_log2
        for b in range(0, size, bs):
            blk = data[b:b + bs].ljust(bs, b"\0")
            k = b // bs
            if hashlib.sha256(blk).digest() != prev[k * 32:(k + 1) * 32]:
                raise ValueError("IVFC level %d block %d hash mismatch" % (i + 1, k))
        prev = data
    return prev  # level 5 = hashes of the RomFS data blocks


def romfs_listing(read):
    hdr = read(0, 0x50)
    (_, dh_o, dh_s, dm_o, dm_s, fh_o, fh_s, fm_o, fm_s, data_off) = struct.unpack_from("<10Q", hdr, 0)
    dmeta = read(dm_o, dm_s)
    fmeta = read(fm_o, fm_s)
    dir_cache = {0: ""}

    def dirpath(o):
        if o not in dir_cache:
            parent, _, _, _, _, name_len = struct.unpack_from("<6I", dmeta, o)
            name = dmeta[o + 0x18:o + 0x18 + name_len].decode("utf-8")
            dir_cache[o] = (dirpath(parent) + "/" + name).lstrip("/")
        return dir_cache[o]

    files = []
    o = 0
    while o < len(fmeta):
        parent, _sib = struct.unpack_from("<II", fmeta, o)
        foff, fsize = struct.unpack_from("<QQ", fmeta, o + 8)
        name_len = struct.unpack_from("<I", fmeta, o + 0x1C)[0]
        name = fmeta[o + 0x20:o + 0x20 + name_len].decode("utf-8")
        d = dirpath(parent)
        files.append(((d + "/" + name) if d else name, data_off + foff, fsize))
        o += (0x20 + name_len + 3) & ~3
    return files


def extract_romfs(reader, fsh, out_dir, log):
    master, levels = ivfc_levels(fsh)
    l5 = verify_small_levels(reader, master, levels)
    l6_off, l6_size, l6_bs_log2 = levels[5]
    bs = 1 << l6_bs_log2
    read6 = lambda o, n: reader.read(l6_off + o, n)
    files = sorted(romfs_listing(read6), key=lambda t: t[1])
    log("RomFS: %d files, %.2f GB, block size 0x%x" % (len(files), l6_size / 1e9, bs))

    made_dirs = set()
    fi, cur, cur_end = 0, None, 0
    bad_blocks = []
    chunk = 64 << 20
    pos = 0
    t0 = time.time()
    while pos < l6_size:
        n = min(chunk, l6_size - pos)
        data = read6(pos, n)
        for b in range(0, n, bs):
            k = (pos + b) // bs
            blk = data[b:b + bs].ljust(bs, b"\0")
            if hashlib.sha256(blk).digest() != l5[k * 32:(k + 1) * 32]:
                bad_blocks.append(k)
        # write every file overlapping [pos, pos + n)
        while True:
            if cur is None:
                if fi >= len(files) or files[fi][1] >= pos + n:
                    break
                path, foff, fsize = files[fi]
                dst = os.path.join(out_dir, *path.split("/"))
                parent = os.path.dirname(dst)
                if parent not in made_dirs:
                    os.makedirs(parent, exist_ok=True)
                    made_dirs.add(parent)
                cur, cur_off, cur_end = open(dst, "wb"), foff, foff + fsize
                fi += 1
                if foff < pos:
                    # Shares data with an earlier file (deduplicated RomFS entry):
                    # fetch the part that has already streamed past.
                    cur.write(read6(foff, min(cur_end, pos) - foff))
            a = max(cur_off, pos)
            e = min(cur_end, pos + n)
            if e > a:
                cur.write(data[a - pos:e - pos])
            if cur_end <= pos + n:
                cur.close()
                cur = None
            else:
                break
        pos += n
        rate = pos / max(time.time() - t0, 1e-6) / 1e6
        log("  %5.1f%%  %.0f MB/s  %d files written" % (100 * pos / l6_size, rate, fi))
    if cur is not None:
        cur.close()
    return {"files": len(files), "bytes": l6_size, "hash_block_size": bs,
            "bad_blocks": bad_blocks, "levels_1_to_5_verified": True}


# ------------------------------------------------------------------ main ---

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--update", required=True)
    ap.add_argument("--keys", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-romfs", action="store_true")
    a = ap.parse_args()
    log = lambda *x, **k: print(*x, **k, flush=True)

    keys = load_keys(a.keys)
    base_nsp, upd_nsp = Nsp(a.base), Nsp(a.update)

    def program_nca(nsp):
        for name, off, size in nsp.ncas():
            nca = Nca(nsp, name, off, size, keys)
            if nca.content_type == "Program":
                return nca
        raise ValueError("no Program NCA in " + nsp.path)

    base, upd = program_nca(base_nsp), program_nca(upd_nsp)
    log("base   %s  title %016x  keygen %d" % (base.name, base.title_id, base.key_generation))
    log("update %s  title %016x  keygen %d" % (upd.name, upd.title_id, upd.key_generation))
    manifest = {"title_id": "%016x" % base.title_id, "base_nca": base.name, "update_nca": upd.name,
                "base_nsp": os.path.basename(a.base), "update_nsp": os.path.basename(a.update)}

    # ExeFS from the update (complete, not a patch)
    exefs = upd.sections[0]
    exe_dir = os.path.join(a.out, "exefs")
    os.makedirs(exe_dir, exist_ok=True)
    pfs_off = struct.unpack_from("<Q", exefs.fsh, 0x8 + 0x38)[0]
    manifest["exefs"] = {}
    for name, off, size in parse_pfs0(exefs.read, pfs_off):
        blob = exefs.read(off, size)
        open(os.path.join(exe_dir, name), "wb").write(blob)
        info = {"size": size, "sha256": hashlib.sha256(blob).hexdigest()}
        if blob[:4] == b"NSO0":
            info["build_id"] = blob[0x40:0x60].hex()
        manifest["exefs"][name] = info
        log("exefs  %-10s %9d bytes" % (name, size))

    if not a.skip_romfs:
        bktr = BktrReader(upd.sections[1], base.sections[1])
        log("BKTR: %d indirect entries, %d AesCtrEx entries, %.1f MB from the patch" %
            (bktr.stats["indirect_entries"], bktr.stats["aesctrex_entries"], bktr.stats["patched_bytes"] / 1e6))
        res = extract_romfs(bktr, upd.sections[1].fsh, os.path.join(a.out, "romfs"), log)
        res.update(bktr.stats)
        manifest["romfs"] = res
        if res["bad_blocks"]:
            log("HASH FAILURES in %d RomFS blocks: %s" % (len(res["bad_blocks"]), res["bad_blocks"][:20]))
        else:
            log("RomFS hash tree verified: every block matches")

    json.dump(manifest, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    return 1 if manifest.get("romfs", {}).get("bad_blocks") else 0


if __name__ == "__main__":
    sys.exit(main())
