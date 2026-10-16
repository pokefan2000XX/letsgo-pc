"""Recover the Maxwell (SM 5.x) instruction table from Ryujinx's shader decoder.

Ryujinx (MIT) ships Ryujinx.Graphics.Shader.dll; its InstTable registers every
instruction as Add("<bit pattern>", InstName.X, ...), and Instructions.cs holds one
struct per instruction whose properties are shift/mask getters over the opcode.
This reads the .NET metadata and IL to print both, as reference data for the
runtime's Maxwell decoder (runtime/src/gpu/maxwell.cpp).

  python -P tools/maxwell_ref.py orig/ryujinx_ref/Ryujinx.Graphics.Shader.dll [--fields]
The DLL is extracted from the user's own Ryujinx/Ryubing single-file build.
"""
import struct
import sys


class Meta:
    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        dd = opt + (96 if magic == 0x10B else 112)
        self.secs = []
        so = opt + opt_size
        for i in range(nsec):
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", data, so + 40 * i + 8)
            self.secs.append((va, max(vsize, raw_size), raw))
        cli_rva = struct.unpack_from("<I", data, dd + 14 * 8)[0]
        cli = self.off(cli_rva)
        md_rva = struct.unpack_from("<I", data, cli + 8)[0]
        md = self.off(md_rva)
        ver_len = struct.unpack_from("<I", data, md + 12)[0]
        o = md + 16 + ver_len + 2
        nstreams = struct.unpack_from("<H", data, o)[0]
        o += 2
        self.streams = {}
        for _ in range(nstreams):
            off, size = struct.unpack_from("<II", data, o)
            o += 8
            e = data.index(b"\0", o)
            name = data[o:e].decode()
            o = (e + 4) & ~3
            self.streams[name] = (md + off, size)
        self.parse_tables()

    def off(self, rva):
        for va, size, raw in self.secs:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(hex(rva))

    def string(self, idx):
        base, _ = self.streams["#Strings"]
        e = self.d.index(b"\0", base + idx)
        return self.d[base + idx:e].decode("utf-8", "replace")

    def us(self, idx):
        base, _ = self.streams["#US"]
        o = base + idx
        b0 = self.d[o]
        if b0 & 0x80 == 0:
            n, o = b0, o + 1
        elif b0 & 0xC0 == 0x80:
            n, o = ((b0 & 0x3F) << 8) | self.d[o + 1], o + 2
        else:
            n, o = ((b0 & 0x1F) << 24) | (self.d[o + 1] << 16) | (self.d[o + 2] << 8) | self.d[o + 3], o + 4
        return self.d[o:o + n - 1].decode("utf-16-le", "replace")

    def parse_tables(self):
        base, _ = self.streams["#~"]
        d = self.d
        heap_sizes = d[base + 6]
        valid = struct.unpack_from("<Q", d, base + 8)[0]
        o = base + 24
        rows = {}
        for t in range(64):
            if valid >> t & 1:
                rows[t] = struct.unpack_from("<I", d, o)[0]
                o += 4
        self.rows = rows
        si = 4 if heap_sizes & 1 else 2
        gi = 4 if heap_sizes & 2 else 2
        bi = 4 if heap_sizes & 4 else 2

        def idx(*tables):
            return 4 if max(rows.get(t, 0) for t in tables) >= 0x10000 else 2

        def coded(tables, bits):
            return 4 if max(rows.get(t, 0) for t in tables if t is not None) >= (1 << (16 - bits)) else 2
        TDR = coded([2, 1, 0x1B], 2)
        RS = coded([0, 0x1A, 0x23, 1], 2)
        HC = coded([4, 8, 0x17], 2)
        MRP = coded([2, 1, 0x1A, 6, 0x1B], 3)
        HCA = coded([6, 4, 1, 2, 8, 9, 0xA, 0, 0xE, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B], 5)
        CAT = coded([None, None, 6, 0xA, None], 3)
        HS = coded([0x14, 0x17], 1)
        MDO = coded([6, 0xA], 1)
        MF = coded([4, 6], 1)
        IMPL = coded([0x26, 0x23, 0x27], 2)
        TOM = coded([2, 6], 1)
        # row layouts (column sizes) of the tables in order
        layouts = {
            0x00: [2, si, gi, gi, gi], 0x01: [RS, si, si], 0x02: [4, si, si, TDR, idx(4), idx(6)],
            0x03: [idx(4)], 0x04: [2, si, bi], 0x05: [idx(6)], 0x06: [4, 2, 2, si, bi, idx(8)], 0x07: [idx(8)],
            0x08: [2, 2, si], 0x09: [idx(2), TDR], 0x0A: [MRP, si, bi], 0x0B: [2, HC, bi], 0x0C: [HCA, CAT, bi],
            0x0D: [HS, bi], 0x0E: [2, HS, bi], 0x0F: [2, 4, idx(2)], 0x10: [4, idx(4)], 0x11: [bi],
            0x12: [idx(2), idx(0x14)], 0x13: [idx(0x14)], 0x14: [2, si, TDR], 0x15: [idx(2), idx(0x17)],
            0x16: [idx(0x17)], 0x17: [2, si, bi], 0x18: [2, idx(6), HS], 0x19: [idx(2), MDO, MDO], 0x1A: [si],
            0x1B: [bi], 0x1C: [2, MF, si, idx(0x1A)], 0x1D: [4, idx(4)], 0x1E: [4, 4], 0x1F: [4],
            0x20: [4, 2, 2, 2, 2, 4, bi, si, si], 0x21: [4], 0x22: [4, 4, 4], 0x23: [2, 2, 2, 2, 4, bi, si, si, bi],
            0x24: [4, idx(0x23)], 0x25: [4, 4, 4, idx(0x23)], 0x26: [4, si, bi], 0x27: [4, 4, si, si, IMPL],
            0x28: [4, 4, si, IMPL], 0x29: [idx(2), idx(2)], 0x2A: [2, 2, TOM, si], 0x2B: [MDO, bi], 0x2C: [idx(0x2A), TDR],
        }
        self.tables = {}
        for t in sorted(rows):
            cols = layouts[t]
            rsize = sum(cols)
            data = []
            for r in range(rows[t]):
                vals = []
                p = o + r * rsize
                for c in cols:
                    vals.append(struct.unpack_from("<H" if c == 2 else "<I", d, p)[0])
                    p += c
                data.append(vals)
            self.tables[t] = data
            o += rsize * rows[t]

    def methods_of(self, ti):
        tds = self.tables[2]
        start = tds[ti][5]
        end = tds[ti + 1][5] if ti + 1 < len(tds) else len(self.tables[6]) + 1
        return range(start, end)

    def fields_of(self, ti):
        tds = self.tables[2]
        start = tds[ti][4]
        end = tds[ti + 1][4] if ti + 1 < len(tds) else len(self.tables[4]) + 1
        return range(start, end)

    def il(self, mi):
        rva = self.tables[6][mi - 1][0]
        if not rva:
            return b""
        o = self.off(rva)
        h = self.d[o]
        if h & 3 == 2:
            return self.d[o + 1:o + 1 + (h >> 2)]
        flags_size, _max, code_size = struct.unpack_from("<HHI", self.d, o)
        return self.d[o + 4 * (flags_size >> 12):o + 4 * (flags_size >> 12) + code_size]


# operand sizes of the one-byte IL opcodes (-1: switch) and of the 0xFE-prefixed ones
_OPSZ = {}
for o in range(0x100):
    _OPSZ[o] = 0
for o in (0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x1F, 0xDE) + tuple(range(0x2B, 0x38)):
    _OPSZ[o] = 1
for o in (0x20, 0x22, 0x27, 0x28, 0x29, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
          0x80, 0x81, 0x8C, 0x8D, 0x8F, 0xA3, 0xA4, 0xA5, 0xC2, 0xC6, 0xD0, 0xDD) + tuple(range(0x38, 0x45)):
    _OPSZ[o] = 4
_OPSZ[0x21] = _OPSZ[0x23] = 8
_OPSZ[0x45] = -1
_FE = {0x06: 4, 0x07: 4, 0x09: 2, 0x0A: 2, 0x0B: 2, 0x0C: 2, 0x0D: 2, 0x0E: 2, 0x12: 1, 0x15: 4, 0x16: 4, 0x19: 1, 0x1C: 4}


def il_ops(code):
    """(opcode, operand bytes) for each IL instruction; 0xFE-prefixed opcodes as 0xFE00 | b"""
    i = 0
    while i < len(code):
        op = code[i]
        i += 1
        if op == 0xFE:
            op2 = code[i]
            i += 1
            n = _FE.get(op2, 0)
            yield 0xFE00 | op2, code[i:i + n]
            i += n
            continue
        n = _OPSZ[op]
        if n == -1:
            cnt = struct.unpack_from("<I", code, i)[0]
            n = 4 + 4 * cnt
        yield op, code[i:i + n]
        i += n


def main():
    m = Meta(open(sys.argv[1], "rb").read())
    tds = m.tables[2]
    names = [m.string(t[1]) for t in tds]
    # InstName enum: field name -> value (Constant table, parent = field)
    consts = {}
    for row in m.tables.get(0x0B, []):
        parent = row[1]
        if parent & 3 == 0:                 # field
            blob_base, _ = m.streams["#Blob"]
            bo = blob_base + row[2]
            n = m.d[bo]
            consts[parent >> 2] = m.d[bo + 1:bo + 1 + n]
    inst_name = {}
    ti = names.index("InstName")
    for fi in m.fields_of(ti):
        v = consts.get(fi)
        if v is not None:
            inst_name[int.from_bytes(v, "little")] = m.string(m.tables[4][fi - 1][1])
    # InstTable..cctor: ldstr "pattern" ... ldc.i4 <InstName>
    ti = names.index("InstTable")
    entries = []
    for mi in m.methods_of(ti):
        pending = None
        last = None
        for op, arg in il_ops(m.il(mi)):
            if op == 0xFE06 and last is not None:          # ldftn InstEmit.<Emitter>: names the variant
                tok = struct.unpack("<I", arg)[0]
                if tok >> 24 == 0x06:
                    entries[last] = entries[last] + (m.string(m.tables[6][(tok & 0xFFFFFF) - 1][3]),)
                last = None
                continue
            if op == 0x72:
                tok = struct.unpack("<I", arg)[0]
                s = m.us(tok & 0xFFFFFF)
                pending = s if s and set(s) <= set("01x") else None
                continue
            if pending is not None:
                val = None
                if op == 0x20:
                    val = struct.unpack("<i", arg)[0]
                elif op == 0x1F:
                    val = struct.unpack("<b", arg)[0]
                elif 0x16 <= op <= 0x1E:
                    val = op - 0x16
                if val is not None:
                    entries.append((pending, inst_name.get(val, "?%d" % val)))
                    last = len(entries) - 1
                    pending = None
    for e in entries:
        print("%-24s %s %s" % (e[0], e[1], e[2] if len(e) > 2 else e[1]))
    if "--fields" in sys.argv:
        # Inst<Name> structs: getters "get_X" computing (opcode >> s) & mask
        for ti, nm in enumerate(names):
            if not nm.startswith("Inst") or nm in ("InstName", "InstTable", "InstProps", "InstOp", "InstConditional"):
                continue
            out = []
            for mi in m.methods_of(ti):
                mn = m.string(m.tables[6][mi - 1][3])
                if not mn.startswith("get_"):
                    continue
                ints = []
                for op, arg in il_ops(m.il(mi)):
                    if op == 0x1F:
                        ints.append(struct.unpack("<b", arg)[0])
                    elif op == 0x20:
                        ints.append(struct.unpack("<i", arg)[0])
                    elif op == 0x21:
                        ints.append(struct.unpack("<q", arg)[0])
                    elif 0x16 <= op <= 0x1E:
                        ints.append(op - 0x16)
                out.append("%s%s" % (mn[4:], ints))
            if out:
                print(nm, " ".join(out))


if __name__ == "__main__":
    main()
