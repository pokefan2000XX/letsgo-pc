"""Census of the game's GPU programs and textures (M3 planning and regression checks).

Scans every place shaders and textures live: loose .bnsh* files, layout archives
(.arc), GFLXPACK entries (decompressed) and the executables. For shaders it counts
the NV_gpu_program5 assembly programs NVN embeds in each binary (header, OPTIONs,
opcodes with modifiers). For BNTX textures it counts the NVN texture formats.

  python -P tools/shader_census.py [--dump DIR]     (--dump writes each unique program as text)
"""
import collections
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfpak  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "orig")


def blobs():
    """(source name, bytes) for every file or pack entry that may hold shaders or textures."""
    for d, _, fs in os.walk(os.path.join(ROOT, "romfs")):
        for f in fs:
            p = os.path.join(d, f)
            low = f.lower()
            if low.endswith(".gfpak"):
                try:
                    for e in gfpak.read(p):
                        yield "%s#%d" % (p, e["index"]), gfpak.data(e)
                except Exception as ex:  # report and continue
                    print("gfpak error", p, ex)
            elif ".bnsh" in low or low.endswith((".arc", ".bntx")):
                yield p, open(p, "rb").read()
    for m in ("main.elf", "sdk.elf"):
        yield m, open(os.path.join(ROOT, "elf", m), "rb").read()


def main():
    dump = sys.argv[sys.argv.index("--dump") + 1] if "--dump" in sys.argv else None
    if dump:
        os.makedirs(dump, exist_ok=True)
    # --dump-ctrl DIR: each unique program's NVN control section (header through the asm
    # text and its program.local constants), as the runtime receives it, for tools/shadertest
    dump_ctrl = sys.argv[sys.argv.index("--dump-ctrl") + 1] if "--dump-ctrl" in sys.argv else None
    if dump_ctrl:
        os.makedirs(dump_ctrl, exist_ok=True)
    ctrl_seen = set()
    heads, opts, ops, sources = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    unique = {}
    tex_formats, bntx_count = collections.Counter(), 0
    for name, b in blobs():
        kind = name.split("#")[0].rsplit(".", 1)[-1].lower()
        if dump_ctrl:
            for m in re.finditer(struct.pack("<I", 0x98761234), b):
                c = m.start()
                if c + 0x24 > len(b):
                    continue
                asm_off, asm_size = struct.unpack_from("<II", b, c + 0x14)
                const_off = struct.unpack_from("<I", b, c + 0x20)[0]
                if b[c + asm_off:c + asm_off + 4] != b"!!NV":
                    continue
                txt = b[c + asm_off:c + asm_off + asm_size]
                h = hashlib.sha1(txt).hexdigest()[:16]
                if h in ctrl_seen:
                    continue
                ctrl_seen.add(h)
                ml = re.search(rb"PARAM c\[(\d+)\] = \{ program\.local", txt)
                end = max(asm_off + asm_size, const_off + 16 * int(ml.group(1)) if ml else 0)
                open(os.path.join(dump_ctrl, h + ".ctrl"), "wb").write(b[c:c + end])
        for m in re.finditer(rb"!!NV[a-z]+[0-9.]+", b):
            end = b.find(b"\0", m.start())
            txt = b[m.start():end].decode("latin1")
            sources[kind] += 1
            h = hashlib.sha1(txt.encode("latin1")).hexdigest()[:16]
            if h in unique:
                continue
            unique[h] = name
            lines = [ln.strip() for ln in txt.split("\n")]
            heads[lines[0]] += 1
            for ln in lines[1:]:
                ln = ln.rstrip(";")
                if not ln or ln == "END":
                    continue
                w = ln.split()[0]
                if w == "OPTION":
                    opts[ln] += 1
                elif w.isupper() and not ln.startswith(("CBUFFER", "ATTRIB", "OUTPUT", "TEMP", "PARAM", "LONG", "SHORT", "INT", "UINT", "FLOAT", "TEXTURE", "BUFFER")):
                    ops[w] += 1
            if dump:
                open(os.path.join(dump, h + ".txt"), "w").write("# " + name + "\n" + txt)
        # BNTX: "BNTX" file, "NX  " block, then BRTI texture infos (u32 format at +0x1C... see below)
        for m in re.finditer(rb"BRTI", b):
            o = m.start()
            if o + 0x30 > len(b):
                continue
            fmt = struct.unpack_from("<I", b, o + 0x1C)[0]
            tex_formats[fmt] += 1
            bntx_count += 1
    print("programs found per container type:", dict(sources))
    print("unique programs:", len(unique), dict(heads))
    print("options:", opts.most_common())
    base = collections.Counter()
    for k, v in ops.items():
        base[k.split(".")[0]] += v
    print("opcode variants (%d):" % len(ops), sorted(ops))
    print("base opcodes (%d):" % len(base), base.most_common())
    print("textures (BRTI): %d, formats (NVN/gfx format id: count):" % bntx_count)
    for f, n in tex_formats.most_common():
        print("  0x%04x  %d" % (f, n))


if __name__ == "__main__":
    main()
