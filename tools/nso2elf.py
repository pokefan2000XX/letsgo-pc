#!/usr/bin/env python3
"""Convert a Switch NSO (main, sdk, subsdk*, rtld) into an ELF64 that Ghidra,
llvm-objdump/readelf and our own tools can load.

The ELF keeps the module's own addresses (vaddr 0 = module start), so pass the
image base to the loader (main is analyzed at 0x7100000000, the usual Switch
convention). Section headers are rebuilt from the MOD0 header and the dynamic
table: .text/.plt, .dynsym/.dynstr, .rela.dyn/.rela.plt, hash tables,
.eh_frame_hdr/.eh_frame, .dynamic, .got/.got.plt, init/fini arrays, .rodata,
.data and .bss. Gaps between known structures become .rodata.N / .data.N.

Usage: python -P tools/nso2elf.py IN.nso OUT.elf [--json OUT.json]
"""
import argparse
import json
import struct
import sys

import lz4.block

DT = {0: "NULL", 1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB",
      7: "RELA", 8: "RELASZ", 9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI",
      14: "SONAME", 16: "SYMBOLIC", 20: "PLTREL", 21: "DEBUG", 22: "TEXTREL", 23: "JMPREL",
      24: "BIND_NOW", 25: "INIT_ARRAY", 26: "FINI_ARRAY", 27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ",
      30: "FLAGS", 0x6ffffef5: "GNU_HASH", 0x6ffffff9: "RELACOUNT", 0x6ffffffb: "FLAGS_1"}
R_AARCH64 = {257: "ABS64", 1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE",
             1028: "TLS_DTPMOD64", 1029: "TLS_DTPREL64", 1030: "TLS_TPREL64", 1031: "TLSDESC"}

SHT_NULL, SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_HASH, SHT_DYNAMIC = 0, 1, 2, 3, 4, 5, 6
SHT_NOBITS, SHT_DYNSYM, SHT_INIT_ARRAY, SHT_FINI_ARRAY, SHT_GNU_HASH = 8, 11, 14, 15, 0x6ffffff6
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR, SHF_INFO_LINK = 1, 2, 4, 0x40


def load_nso(path):
    blob = open(path, "rb").read()
    if blob[:4] != b"NSO0":
        raise ValueError("not an NSO")
    flags = struct.unpack_from("<I", blob, 0xC)[0]
    segs = []
    for i, so in enumerate((0x10, 0x20, 0x30)):
        foff, moff, dsize = struct.unpack_from("<III", blob, so)
        csize = struct.unpack_from("<I", blob, 0x60 + 4 * i)[0]
        data = blob[foff:foff + csize]
        if flags & (1 << i):
            data = lz4.block.decompress(data, uncompressed_size=dsize)
        segs.append((moff, data))
    bss = struct.unpack_from("<I", blob, 0x3C)[0]
    image = bytearray(max(m + len(d) for m, d in segs))
    for m, d in segs:
        image[m:m + len(d)] = d
    ro_off = segs[1][0]
    dynstr = struct.unpack_from("<II", blob, 0x90)
    dynsym = struct.unpack_from("<II", blob, 0x98)
    return {
        "image": image, "build_id": blob[0x40:0x60].hex(),
        "text": (segs[0][0], len(segs[0][1])), "ro": (segs[1][0], len(segs[1][1])),
        "data": (segs[2][0], len(segs[2][1])), "bss_size": bss,
        "dynstr": (ro_off + dynstr[0], dynstr[1]), "dynsym": (ro_off + dynsym[0], dynsym[1]),
    }


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def s32(b, o): return struct.unpack_from("<i", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def analyze(nso):
    img = nso["image"]
    mod0 = u32(img, 4)
    if img[mod0:mod0 + 4] != b"MOD0":
        raise ValueError("no MOD0 header")
    rel = [s32(img, mod0 + 4 + 4 * i) for i in range(6)]
    info = {"mod0": mod0, "dynamic": mod0 + rel[0], "bss_start": mod0 + rel[1], "bss_end": mod0 + rel[2],
            "eh_frame_hdr": (mod0 + rel[3], rel[4] - rel[3]), "module_object": mod0 + rel[5]}
    dyn, o = {}, info["dynamic"]
    needed = []
    while True:
        tag, val = struct.unpack_from("<qQ", img, o)
        o += 16
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        dyn[tag] = val
    info["dynamic_size"] = o - info["dynamic"]
    info["dyn"] = {DT.get(k, hex(k)): v for k, v in dyn.items()}
    strtab, strsz = dyn[5], dyn[10]

    def cstr(off):
        a = strtab + off
        return img[a:img.index(b"\0", a)].decode("latin1")

    info["needed"] = [cstr(n) for n in needed]
    # symbols
    symtab = dyn[6]
    nsyms = nso["dynsym"][1] // 24
    syms = []
    for i in range(nsyms):
        name, inf, oth, shndx, val, size = struct.unpack_from("<IBBHQQ", img, symtab + i * 24)
        syms.append({"name": cstr(name) if name else "", "bind": inf >> 4, "type": inf & 0xF,
                     "shndx": shndx, "value": val, "size": size})
    info["symbols"] = syms
    # relocations
    relocs = []
    for tag_off, tag_sz, kind in ((7, 8, "rela.dyn"), (23, 2, "rela.plt")):
        if tag_off in dyn:
            base, size = dyn[tag_off], dyn[tag_sz]
            for r in range(0, size, 24):
                off, rinfo, addend = struct.unpack_from("<QQq", img, base + r)
                relocs.append({"table": kind, "offset": off, "type": rinfo & 0xFFFFFFFF,
                               "sym": rinfo >> 32, "addend": addend})
    info["relocs"] = relocs
    # .hash / .gnu.hash sizes
    if 4 in dyn:
        nb, nc = struct.unpack_from("<II", img, dyn[4])
        info["hash_size"] = (2 + nb + nc) * 4
    if 0x6ffffef5 in dyn:
        g = dyn[0x6ffffef5]
        nb, symoff, bloom, _shift = struct.unpack_from("<IIII", img, g)
        buckets = g + 16 + bloom * 8
        chains = buckets + nb * 4
        last = max(struct.unpack_from("<%dI" % nb, img, buckets)) if nb else 0
        if last >= symoff:
            while not (u32(img, chains + (last - symoff) * 4) & 1):
                last += 1
            info["gnu_hash_size"] = chains + (last - symoff + 1) * 4 - g
        else:
            info["gnu_hash_size"] = chains - g
    # .eh_frame from .eh_frame_hdr
    ehh, ehh_size = info["eh_frame_hdr"]
    if ehh_size:
        ver, ptr_enc, cnt_enc, tbl_enc = img[ehh:ehh + 4]
        if ptr_enc == 0x1B:  # pcrel | sdata4
            eh = ehh + 4 + s32(img, ehh + 4)
            p = eh
            while True:
                length = u32(img, p)
                if length == 0:
                    p += 4
                    break
                p += 4 + length
            info["eh_frame"] = (eh, p - eh)
    # PLT: stubs are ADRP x16 / LDR x17,[x16,#off] / ADD x16,x16,#off / BR x17
    got_slots = {r["offset"]: syms[r["sym"]]["name"] for r in relocs if r["type"] == 1026}
    plt = {}
    t0, tsz = nso["text"]
    for a in range(t0, t0 + tsz - 12, 4):
        w0, w1, w3 = u32(img, a), u32(img, a + 4), u32(img, a + 12)
        if (w0 & 0x9F00001F) == 0x90000010 and (w1 & 0xFFC003FF) == 0xF9400211 and w3 == 0xD61F0220:
            immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7FFFF
            page = ((a & ~0xFFF) + (((immhi << 2) | immlo) << 12)) & 0xFFFFFFFFFFFF
            if page & (1 << 47):
                page -= 1 << 48
            slot = page + ((w1 >> 10) & 0xFFF) * 8
            if slot in got_slots:
                plt[a] = got_slots[slot]
    info["plt"] = plt
    return info


def build_sections(nso, info):
    """Non-overlapping section list: (name, type, flags, addr, size, link, info, entsize, align)."""
    img = nso["image"]
    dyn = info["dyn"]
    t0, tsz = nso["text"]
    r0, rsz = nso["ro"]
    d0, dsz = nso["data"]
    secs = []
    # text / plt
    if info["plt"]:
        p0 = min(info["plt"]) - 0x20   # PLT0 header
        p1 = max(info["plt"]) + 0x10
        if t0 < p0:
            secs.append([".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, t0, p0 - t0, 0, 0, 0, 16])
        secs.append([".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, p0, p1 - p0, 0, 0, 16, 16])
        if p1 < t0 + tsz:
            secs.append([".text.1", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, p1, t0 + tsz - p1, 0, 0, 0, 16])
    else:
        secs.append([".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, t0, tsz, 0, 0, 0, 16])

    known_ro = [(".dynsym", SHT_DYNSYM, nso["dynsym"][0], nso["dynsym"][1], 24),
                (".dynstr", SHT_STRTAB, dyn["STRTAB"], dyn["STRSZ"], 0)]
    if "RELA" in dyn:
        known_ro.append((".rela.dyn", SHT_RELA, dyn["RELA"], dyn["RELASZ"], 24))
    if "JMPREL" in dyn:
        known_ro.append((".rela.plt", SHT_RELA, dyn["JMPREL"], dyn["PLTRELSZ"], 24))
    if "hash_size" in info:
        known_ro.append((".hash", SHT_HASH, dyn["HASH"], info["hash_size"], 4))
    if "gnu_hash_size" in info:
        known_ro.append((".gnu.hash", SHT_GNU_HASH, dyn["GNU_HASH"], info["gnu_hash_size"], 0))
    if info["eh_frame_hdr"][1]:
        known_ro.append((".eh_frame_hdr", SHT_PROGBITS, info["eh_frame_hdr"][0], info["eh_frame_hdr"][1], 0))
    if "eh_frame" in info:
        known_ro.append((".eh_frame", SHT_PROGBITS, info["eh_frame"][0], info["eh_frame"][1], 0))

    got_dat = sorted(r["offset"] for r in info["relocs"] if r["type"] == 1025)
    got_plt = sorted(r["offset"] for r in info["relocs"] if r["type"] == 1026)
    known_rw = [(".dynamic", SHT_DYNAMIC, info["dynamic"], info["dynamic_size"], 16)]
    if "INIT_ARRAY" in dyn:
        known_rw.append((".init_array", SHT_INIT_ARRAY, dyn["INIT_ARRAY"], dyn["INIT_ARRAYSZ"], 8))
    if "FINI_ARRAY" in dyn:
        known_rw.append((".fini_array", SHT_FINI_ARRAY, dyn["FINI_ARRAY"], dyn["FINI_ARRAYSZ"], 8))
    if got_plt and "PLTGOT" in dyn:
        known_rw.append((".got.plt", SHT_PROGBITS, dyn["PLTGOT"], got_plt[-1] + 8 - dyn["PLTGOT"], 8))
    if got_dat:
        known_rw.append((".got", SHT_PROGBITS, got_dat[0], got_dat[-1] + 8 - got_dat[0], 8))

    def tile(known, seg0, segsz, filler, flags):
        # Keep known structures that fall inside this segment and don't overlap; fill gaps.
        inside = sorted((k for k in known if seg0 <= k[2] and k[2] + k[3] <= seg0 + segsz and k[3] > 0),
                        key=lambda k: (k[2], -k[3]))
        out, cur, n = [], seg0, 0
        for name, typ, addr, size, ent in inside:
            if addr < cur:
                continue  # overlaps the previous structure; leave it to the segment
            if addr > cur:
                out.append([filler + ("" if n == 0 else ".%d" % n), SHT_PROGBITS, flags, cur, addr - cur, 0, 0, 0, 8])
                n += 1
            out.append([name, typ, flags, addr, size, 0, 0, ent, 8])
            cur = addr + size
        if cur < seg0 + segsz:
            out.append([filler + ("" if n == 0 else ".%d" % n), SHT_PROGBITS, flags, cur, seg0 + segsz - cur, 0, 0, 0, 8])
        return out

    secs += tile(known_ro, r0, rsz, ".rodata", SHF_ALLOC)
    secs += tile(known_rw, d0, dsz, ".data", SHF_ALLOC | SHF_WRITE)
    bss0 = d0 + dsz
    bss_end = max(info["bss_end"], bss0 + nso["bss_size"])
    secs.append([".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss0, bss_end - bss0, 0, 0, 0, 16])
    # links: dynsym -> dynstr, rela -> dynsym, hash -> dynsym, dynamic -> dynstr
    idx = {s[0]: i + 1 for i, s in enumerate(secs)}
    for s in secs:
        if s[0] in (".dynsym", ".dynamic"):
            s[5] = idx.get(".dynstr", 0)
            if s[0] == ".dynsym":
                s[6] = 1
        elif s[0] in (".rela.dyn", ".rela.plt", ".hash", ".gnu.hash"):
            s[5] = idx.get(".dynsym", 0)
        if s[0] == ".rela.plt" and ".got.plt" in idx:
            s[6] = idx[".got.plt"]
            s[2] |= SHF_INFO_LINK
    return secs, bss_end


def write_elf(nso, info, out_path):
    img = nso["image"]
    secs, bss_end = build_sections(nso, info)
    t0, tsz = nso["text"]
    r0, rsz = nso["ro"]
    d0, dsz = nso["data"]
    IMG_FOFF = 0x1000
    phdrs = [
        (1, 5, t0, tsz, tsz, 0x1000),            # PT_LOAD R+X
        (1, 4, r0, rsz, rsz, 0x1000),            # PT_LOAD R
        (1, 6, d0, dsz, bss_end - d0, 0x1000),   # PT_LOAD RW
        (2, 6, info["dynamic"], info["dynamic_size"], info["dynamic_size"], 8),  # PT_DYNAMIC
    ]
    if info["eh_frame_hdr"][1]:
        a, n = info["eh_frame_hdr"]
        phdrs.append((0x6474E550, 4, a, n, n, 4))  # PT_GNU_EH_FRAME
    shstr = bytearray(b"\0")
    name_off = []
    for s in secs + [[".shstrtab"]]:
        name_off.append(len(shstr))
        shstr += s[0].encode() + b"\0"
    body = bytes(img[:d0 + dsz])
    shstr_foff = IMG_FOFF + len(body)
    sh_foff = (shstr_foff + len(shstr) + 7) & ~7
    out = bytearray(sh_foff + 64 * (len(secs) + 2))
    struct.pack_into("<4sBBBBB7sHHIQQQIHHHHHH", out, 0, b"\x7fELF", 2, 1, 1, 0, 0, b"\0" * 7,
                     3, 183, 1, 0, 0x40, sh_foff, 0, 64, 56, len(phdrs), 64, len(secs) + 2, len(secs) + 1)
    for i, (typ, fl, va, fsz, msz, al) in enumerate(phdrs):
        struct.pack_into("<IIQQQQQQ", out, 0x40 + i * 56, typ, fl, IMG_FOFF + va, va, va, fsz, msz, al)
    out[IMG_FOFF:IMG_FOFF + len(body)] = body
    out[shstr_foff:shstr_foff + len(shstr)] = shstr
    for i, (name, typ, flags, addr, size, link, inf, ent, align) in enumerate(secs):
        foff = IMG_FOFF + addr if typ != SHT_NOBITS else IMG_FOFF + d0 + dsz
        struct.pack_into("<IIQQQQIIQQ", out, sh_foff + 64 * (i + 1), name_off[i], typ, flags, addr,
                         foff, size, link, inf, align, ent)
    struct.pack_into("<IIQQQQIIQQ", out, sh_foff + 64 * (len(secs) + 1), name_off[-1], SHT_STRTAB, 0, 0,
                     shstr_foff, len(shstr), 0, 0, 1, 0)
    # Dynamic symbols carry section indices from Nintendo's original link; point
    # them at the rebuilt section that contains each symbol (or SHN_ABS).
    starts = sorted((s[3], s[3] + max(s[4], 1), i + 1) for i, s in enumerate(secs) if s[1] != SHT_NULL)
    sym_off = IMG_FOFF + nso["dynsym"][0]
    for k in range(nso["dynsym"][1] // 24):
        o = sym_off + k * 24
        shndx = struct.unpack_from("<H", out, o + 6)[0]
        if shndx == 0 or shndx >= 0xFF00:
            continue
        val = struct.unpack_from("<Q", out, o + 8)[0]
        idx = next((i for a0, a1, i in starts if a0 <= val < a1), 0xFFF1)
        struct.pack_into("<H", out, o + 6, idx)
    open(out_path, "wb").write(out)
    return secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("elf")
    ap.add_argument("--json")
    a = ap.parse_args()
    nso = load_nso(a.nso)
    info = analyze(nso)
    secs = write_elf(nso, info, a.elf)
    rtypes = {}
    for r in info["relocs"]:
        k = R_AARCH64.get(r["type"], str(r["type"]))
        rtypes[k] = rtypes.get(k, 0) + 1
    summary = {
        "build_id": nso["build_id"],
        "segments": {k: {"addr": nso[k][0], "size": nso[k][1]} for k in ("text", "ro", "data")},
        "bss": {"addr": nso["data"][0] + nso["data"][1], "end": max(info["bss_end"], nso["data"][0] + nso["data"][1] + nso["bss_size"])},
        "mod0": info["mod0"], "module_object": info["module_object"],
        "dynamic": info["dyn"], "needed": info["needed"],
        "eh_frame_hdr": info["eh_frame_hdr"], "eh_frame": info.get("eh_frame"),
        "symbols": {"total": len(info["symbols"]),
                    "imports": sum(1 for s in info["symbols"] if s["shndx"] == 0 and s["name"]),
                    "exports": sum(1 for s in info["symbols"] if s["shndx"] != 0 and s["name"])},
        "relocations": rtypes, "plt_stubs": len(info["plt"]),
        "sections": [{"name": s[0], "addr": s[3], "size": s[4]} for s in secs],
    }
    if a.json:
        json.dump(summary, open(a.json, "w"), indent=1)
    print(json.dumps({k: summary[k] for k in ("build_id", "segments", "bss", "symbols", "relocations", "plt_stubs")}))


if __name__ == "__main__":
    sys.exit(main())
