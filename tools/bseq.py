#!/usr/bin/env python3
"""Let's Go's battle sequences (.bseq: move animations and other battle effects): read, to JSON and back.

  python -P tools/bseq.py info <file.bseq>                 header and commands, one per line
  python -P tools/bseq.py dump <file.bseq> [-o out.json]   editable JSON (stdout without -o)
  python -P tools/bseq.py build <file.json> -o <file.bseq> back to a sequence the game plays
  python -P tools/bseq.py roundtrip [file|dir ...]         read, dump and build every sequence, compare bytes
  python -P tools/bseq.py commands                         each command: its parameters' size and types, uses

The format (docs/SEQUENCES.md has it in full): a 16-byte header ("SESD", version 3, 0, the last frame), then commands
sorted by start frame, each a start and an end frame (s32), three u16 values, the command's number (u16) and its
parameters, and -1 after the last. The file doesn't give a command's parameter size: the game has a table of them
(227 commands, read from the player's own orig/exefs/main), so this tool reads that table. The JSON gives each
parameter the type the game's code reads it as (SIGNATURES). Everything printed is derived from the game: keep it on
this machine.
"""
import argparse
import glob
import json
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "orig", "analysis", "bseq_sizes.json")
SEQUENCES = os.path.join(ROOT, "orig", "romfs", "bin", "battle", "waza", "sequence")
BASE = 0x7100000000
SIZE_TABLE = 0x7100f97e30      # s32 per command: its parameters' size in bytes (passed to the player by 0x7100aa0c60)
HANDLERS = 0x71015d4c70        # the battle's handler for each command (0x7100ada060 on), not needed to read the files
SECOND = 0x71015d5388          # a second table, run by a second handler (0x7100aa2290): code for 54, 77, 78 and 188
COUNT = 227
MAGIC = b"SESD"
HEADER = struct.Struct("<4sIII")
COMMAND = struct.Struct("<iiHHHH")

# What a command does, where the strings in its parameters or its code make it plain (letsgostudio names them the same).
KNOWN = {
    17: "character animation",
    18: "character animation",
    44: "camera animation file",
    46: "camera animation file",
    54: "particle file",
    58: "particles to a Pokemon",
    62: "particles' scale",
    77: "model file",
    103: "model animation list",
    106: "model name",
    112: "sound (Wwise event)",
    113: "two sounds (Wwise events)",
    115: "sequence name",
    170: "character animation",
}


class SequenceError(Exception):
    pass


def sizes():
    """[s32] * COUNT: each command's parameter size, from the game (cached in orig/analysis)."""
    if os.path.exists(CACHE):
        return json.load(open(CACHE))
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import nso2elf
    img = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))["image"]
    out = list(struct.unpack_from("<%di" % COUNT, img, SIZE_TABLE - BASE))
    if any(s < 0 or s % 4 for s in out):
        raise SequenceError("orig/exefs/main isn't the version this tool reads (1.0.2): its size table looks wrong")
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(out, open(CACHE, "w"))
    return out


def parse(data, size=None):
    """{"version", "unknown", "frames", "commands": [{"start", "end", "a", "b", "c", "command", "params": bytes}]}."""
    size = size or sizes()
    if len(data) < HEADER.size + 4 or data[:4] != MAGIC:
        raise SequenceError("not a battle sequence (no SESD header)")
    magic, version, unknown, frames = HEADER.unpack_from(data, 0)
    if version != 3:
        raise SequenceError("version %d: only version 3 (every file in the game) is read" % version)
    pos, out = HEADER.size, []
    while True:
        if pos + 4 > len(data):
            raise SequenceError("the commands run past the end of the file (no -1 after the last)")
        if struct.unpack_from("<i", data, pos)[0] == -1:
            pos += 4
            break
        if pos + COMMAND.size > len(data):
            raise SequenceError("a command at %#x runs past the end of the file" % pos)
        start, end, a, b, c, cid = COMMAND.unpack_from(data, pos)
        if cid >= len(size):
            raise SequenceError("command %d at %#x: the game has %d" % (cid, pos, len(size)))
        params = data[pos + COMMAND.size:pos + COMMAND.size + size[cid]]
        if len(params) != size[cid]:
            raise SequenceError("command %d at %#x runs past the end of the file" % (cid, pos))
        out.append({"start": start, "end": end, "a": a, "b": b, "c": c, "command": cid, "params": params})
        pos += COMMAND.size + size[cid]
    if pos != len(data):
        raise SequenceError("%d bytes after the end of the commands" % (len(data) - pos))
    return {"version": version, "unknown": unknown, "frames": frames, "commands": out}


def build(seq, size=None):
    """The file for a parsed sequence. Commands are kept sorted by start frame (stable): the game runs a command only
    when the frame it's at is its start, walking the list in order, so one out of order would stop the rest."""
    size = size or sizes()
    out = bytearray(HEADER.pack(MAGIC, seq.get("version", 3), seq.get("unknown", 0), seq["frames"]))
    for c in sorted(seq["commands"], key=lambda c: c["start"]):
        cid = c["command"]
        if not 0 <= cid < len(size):
            raise SequenceError("command %d: the game has %d (0 to %d)" % (cid, len(size), len(size) - 1))
        if len(c["params"]) != size[cid]:
            raise SequenceError("command %d takes %d bytes of parameters, not %d" % (cid, size[cid], len(c["params"])))
        if c["start"] < 0 or c["end"] < c["start"]:
            raise SequenceError("command %d: frames %d to %d (a start of 0 or more, an end not before it)"
                                % (cid, c["start"], c["end"]))
        out += COMMAND.pack(c["start"], c["end"], c["a"], c["b"], c["c"], cid) + bytes(c["params"])
    out += struct.pack("<i", -1)
    return bytes(out)


# ---- parameters as values ------------------------------------------------------------------------------------------
# Each command's parameters in the order the game's code reads them, one letter per read: i an int, f a float, v three
# floats (x, y, z), b a bool (4 bytes, true when not 0), h a 64-bit value, s<N> a text in N bytes (ended by a NUL),
# x 4 bytes no code reads. From the readers each command's code calls in 1.0.2: the function that plays it (HANDLERS),
# the second table's (SECOND: commands 54, 77, 78 and 188) and running up to a frame at once (0x7100aa0eb0's switch:
# 20, 44 to 48, 54, 77, 78, 103, 115 and 188). Where more than one reads a command they agree, and every signature adds
# up to the size table's. The commands not listed take no parameters (23 of them) or have only bytes no code reads (70;
# three of those are in the game's files, 11, 189 and 208).
SIGNATURES = {
    2: "i v b b b i b b b", 3: "i i i v i b b b b i b b b", 4: "i i s32 v b b i", 5: "i i", 6: "i", 7: "i v b i",
    8: "i v b b i", 9: "i i i f i", 10: "i i v b b f i", 12: "i b", 13: "b", 14: "b", 15: "i b", 16: "i i",
    17: "i s64 b", 18: "i s64 b", 20: "i i", 21: "i f", 24: "i v f v f", 26: "i i s32 b b b v v", 27: "i",
    28: "i f f f f i b", 30: "i b b", 32: "i i v", 35: "v v f i b b b b b b b", 36: "i i v v f i b b b b i b b b b b b",
    37: "i v v f b b i", 38: "i", 39: "i i i i i", 40: "i i i i i", 41: "i i", 42: "f f f i b b", 43: "f b b i i",
    44: "s256 s256 i i v b b b b", 45: "s256 s256 v b", 46: "i s256 s256 i s32 v b b b b b", 48: "s256 s256 i v b b",
    49: "b", 50: "f", 51: "v", 52: "f", 54: "s256 i b i i i", 57: "v b i b b b", 58: "i i v i b b b b b i b b b",
    59: "i v b b x", 60: "i s32 i", 61: "i s32 v b b i", 62: "v b i", 63: "v b i", 64: "i i f b i", 65: "i s32 f b i",
    66: "v b i", 67: "i i f b b b i", 68: "i s32 b b b v v", 69: "i i i v b b", 71: "b", 72: "f v", 73: "b v b i",
    75: "f f f f i", 77: "s256 b i i b", 78: "i i i i", 80: "v b i b b b", 81: "i i v i b b b b b i b b b",
    82: "i v b b x", 83: "i s32 v b b i", 84: "v b i", 85: "v b i", 86: "i i f b i", 87: "i s32 f b i", 88: "v b i",
    89: "i i f b b b i", 90: "b", 91: "i s32 b b b v v", 92: "i i i v b b", 94: "i i s32 b", 95: "b", 96: "f v",
    98: "b v b i", 99: "s32 s32 v", 100: "s32 s32 f", 101: "s32 s32 b", 102: "s256 b i", 103: "s256", 104: "i",
    105: "f i", 106: "s64", 110: "f f f f i", 112: "s64 b", 113: "s64 s64 b", 114: "s64", 115: "s64", 116: "i i i",
    117: "v b i", 118: "i i v i i", 119: "i v b b", 120: "i i i v", 121: "i i v", 122: "f v", 124: "i b b", 125: "b",
    126: "i", 127: "i", 128: "i i", 130: "b v f", 131: "v f", 132: "b b", 133: "i", 134: "v f b i", 136: "b v b i",
    139: "i i v", 140: "b f f i", 161: "i v b i", 162: "i i i v i b b b b b i b b b", 163: "i b", 164: "b",
    165: "i v b i", 166: "i b", 167: "b", 168: "b", 169: "i b", 170: "i s64", 178: "h i b", 179: "h i", 181: "i",
    184: "i b", 185: "i b", 186: "i b", 188: "i i i i i i i i i i i i", 198: "i i", 199: "i i", 200: "i b", 202: "i b",
    203: "v v b", 204: "i b",
}
KINDS = {"i": 4, "f": 4, "v": 12, "b": 4, "h": 8, "x": 4}
NAMES = {"i": "int", "f": "float", "v": "[x, y, z]", "b": "true or false", "h": "64-bit value", "x": "int", "s": "text"}


def fields(cid, size=None):
    """[(kind, offset, bytes)]: how a command's parameters are read. Its signature, when that adds up to the size
    table's (another version of the game might not), else 4-byte ints."""
    size = size or sizes()
    n = size[cid] if 0 <= cid < len(size) else 0
    out, k = [], 0
    for token in SIGNATURES.get(cid, "").split():
        width = int(token[1:]) if token[0] == "s" else KINDS[token]
        out.append((token[0], k, width))
        k += width
    if k != n:
        out = [("x", j, 4) for j in range(0, n, 4)]
    return out


def f32(w):
    """The shortest decimal that is the same single-precision float (0.3, not 0.30000001192092896)."""
    x = struct.unpack("<f", struct.pack("<I", w))[0]
    for digits in range(6, 10):
        y = float("%.*g" % (digits, x))
        try:
            if struct.pack("<f", y) == struct.pack("<I", w):
                return y
        except OverflowError:
            pass
    return x


def values(params, flds):
    """The parameters as JSON values by their fields: ints, floats, [x, y, z], true or false and texts. None when one
    holds something its kind can't show (a float that isn't finite, a bool other than 0 or 1, a text with bytes after
    its NUL; none of the game's files has one), so the caller writes the bytes as ints."""
    out = []
    for kind, k, n in flds:
        chunk = params[k:k + n]
        if kind in "ix":
            out.append(struct.unpack("<i", chunk)[0])
        elif kind == "h":
            out.append(struct.unpack("<Q", chunk)[0])
        elif kind == "b":
            w = struct.unpack("<I", chunk)[0]
            if w > 1:
                return None
            out.append(w == 1)
        elif kind in "fv":
            words = struct.unpack("<%dI" % (n // 4), chunk)
            if any(w & 0x7f800000 == 0x7f800000 for w in words):
                return None
            xs = [f32(w) for w in words]
            out.append(xs[0] if kind == "f" else xs)
        else:
            nul = chunk.find(b"\0")
            if nul < 0 or any(chunk[nul:]) or not all(0x20 <= x < 0x7f for x in chunk[:nul]):
                return None
            out.append(chunk[:nul].decode("ascii"))
    return out


def float32(v, where):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        raise SequenceError("%s: a number, not %r" % (where, v))
    try:
        return struct.pack("<f", v)
    except OverflowError:
        raise SequenceError("%s: %r is too large for a float" % (where, v))


def pack(vals, flds, cid):
    """The bytes for a command's parameter values, each checked against its field."""
    if not isinstance(vals, list) or len(vals) != len(flds):
        raise SequenceError("command %d takes %d values (%s), not %s" % (
            cid, len(flds), ", ".join(NAMES[f[0]] for f in flds) or "none",
            len(vals) if isinstance(vals, list) else repr(vals)))
    out = bytearray()
    for i, (v, (kind, k, n)) in enumerate(zip(vals, flds)):
        where = "command %d, value %d" % (cid, i + 1)
        if kind in "ix":
            if isinstance(v, bool) or not isinstance(v, int) or not -0x80000000 <= v <= 0xffffffff:
                raise SequenceError("%s: an int (32 bits), not %r" % (where, v))
            out += struct.pack("<I", v & 0xffffffff)
        elif kind == "h":
            if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v < 1 << 64:
                raise SequenceError("%s: a 64-bit value (0 or more), not %r" % (where, v))
            out += struct.pack("<Q", v)
        elif kind == "b":
            if not (isinstance(v, bool) or (type(v) is int and v in (0, 1))):
                raise SequenceError("%s: true or false, not %r" % (where, v))
            out += struct.pack("<I", int(v))
        elif kind == "f":
            out += float32(v, where)
        elif kind == "v":
            if not isinstance(v, list) or len(v) != 3:
                raise SequenceError("%s: [x, y, z], not %r" % (where, v))
            for x in v:
                out += float32(x, where)
        else:
            if not isinstance(v, str) or not all(0x20 <= ord(ch) < 0x7f for ch in v):
                raise SequenceError("%s: a text (letters, numbers and signs), not %r" % (where, v))
            if len(v) + 1 > n:
                raise SequenceError("%s: %r is longer than its %d bytes allow (%d characters at most)"
                                    % (where, v, n, n - 1))
            out += v.encode("ascii") + bytes(n - len(v))
    return bytes(out)


def pack_raw(vals, cid):
    """The bytes for "raw": 4-byte ints, as they were in the file."""
    if not isinstance(vals, list):
        raise SequenceError("command %d: raw is a list of ints, not %r" % (cid, vals))
    out = bytearray()
    for v in vals:
        if isinstance(v, bool) or not isinstance(v, int) or not -0x80000000 <= v <= 0xffffffff:
            raise SequenceError("command %d: raw holds ints (32 bits), not %r" % (cid, v))
        out += struct.pack("<I", v & 0xffffffff)
    return bytes(out)


def to_json(seq, size=None):
    size = size or sizes()
    commands = []
    for c in seq["commands"]:
        cid = c["command"]
        item = {"start": c["start"], "end": c["end"], "a": c["a"], "b": c["b"], "c": c["c"], "command": cid}
        if cid in KNOWN:
            item["what"] = KNOWN[cid]
        flds = fields(cid, size)
        vals = values(c["params"], flds)
        if vals is not None and pack(vals, flds, cid) == c["params"]:
            item["params"] = vals
        else:       # a value its kind can't show: the bytes as ints, built back as they are
            item["raw"] = list(struct.unpack("<%di" % (len(c["params"]) // 4), c["params"]))
        commands.append(item)
    return {"version": seq["version"], "unknown": seq["unknown"], "frames": seq["frames"], "commands": commands}


def from_json(obj, size=None):
    size = size or sizes()
    commands = []
    for i, c in enumerate(obj["commands"]):
        try:
            cid = c["command"]
            if isinstance(cid, bool) or not isinstance(cid, int) or not 0 <= cid < len(size):
                raise SequenceError("command %r: the game has %d (0 to %d)" % (cid, len(size), len(size) - 1))
            params = pack_raw(c["raw"], cid) if "raw" in c else pack(c.get("params", []), fields(cid, size), cid)
            commands.append({"start": c["start"], "end": c["end"], "a": c.get("a", 0), "b": c.get("b", 0),
                             "c": c.get("c", 0), "command": cid, "params": params})
        except KeyError as e:
            raise SequenceError("command %d in the list has no %s" % (i, e))
    return {"version": obj.get("version", 3), "unknown": obj.get("unknown", 0), "frames": obj["frames"],
            "commands": commands}


def dumps(obj):
    """JSON with one command per line, its parameters inline."""
    head = {k: v for k, v in obj.items() if k != "commands"}
    lines = ["{"] + ['  %s: %s,' % (json.dumps(k), json.dumps(v)) for k, v in head.items()]
    lines.append('  "commands": [')
    for i, c in enumerate(obj["commands"]):
        lines.append("    " + json.dumps(c, ensure_ascii=False) + ("," if i + 1 < len(obj["commands"]) else ""))
    lines += ["  ]", "}"]
    return "\n".join(lines) + "\n"


def shown(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return "%g" % v
    if isinstance(v, list):
        return "(%s)" % ", ".join("%g" % x for x in v)
    if isinstance(v, str):
        return json.dumps(v)
    return str(v)


def summary(c):
    """One line for a command: frames, its three values, its number and name, its parameters."""
    what = KNOWN.get(c["command"], "")
    vals = [shown(v) for v in c["params"]] if "params" in c else ["raw"] + [str(v) for v in c["raw"]]
    return "%5d-%-5d a %-3d b %-3d c %d  cmd %3d %-26s %s" % (
        c["start"], c["end"], c["a"], c["b"], c["c"], c["command"], what, " ".join(vals))


def sequence_files(paths):
    out = []
    for p in paths or [SEQUENCES]:
        if os.path.isdir(p):
            out += sorted(glob.glob(os.path.join(p, "*.bseq")))
        else:
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("file")
    p = sub.add_parser("dump")
    p.add_argument("file")
    p.add_argument("-o")
    p = sub.add_parser("build")
    p.add_argument("file")
    p.add_argument("-o", required=True)
    p = sub.add_parser("roundtrip")
    p.add_argument("paths", nargs="*")
    sub.add_parser("commands")
    a = ap.parse_args()
    try:
        if a.cmd == "info":
            seq = parse(open(a.file, "rb").read())
            obj = to_json(seq)
            print("version %d, %d frames, %d commands" % (seq["version"], seq["frames"], len(seq["commands"])))
            for c in obj["commands"]:
                print(summary(c).rstrip())
        elif a.cmd == "dump":
            text = dumps(to_json(parse(open(a.file, "rb").read())))
            if a.o:
                open(a.o, "w", encoding="utf-8", newline="\n").write(text)
            else:
                sys.stdout.buffer.write(text.encode("utf-8"))
        elif a.cmd == "build":
            data = build(from_json(json.load(open(a.file, encoding="utf-8"))))
            open(a.o, "wb").write(data)
        elif a.cmd == "roundtrip":
            files = sequence_files(a.paths)
            same = 0
            for f in files:
                data = open(f, "rb").read()
                try:
                    seq = parse(data)
                    again = build(from_json(json.loads(dumps(to_json(seq)))))
                    if build(seq) == data and again == data:
                        same += 1
                    else:
                        print("differs:", f)
                except SequenceError as e:
                    print("%s: %s" % (f, e))
            print("%d of %d sequences come back byte for byte" % (same, len(files)))
            if same != len(files):
                sys.exit(1)
        elif a.cmd == "commands":
            size = sizes()
            uses = {}
            for f in sequence_files([]):
                for c in parse(open(f, "rb").read(), size)["commands"]:
                    uses[c["command"]] = uses.get(c["command"], 0) + 1
            for cid in range(len(size)):
                flds = fields(cid, size)
                shape = " ".join("s%d" % f[2] if f[0] == "s" else f[0] for f in flds)
                if flds and all(f[0] == "x" for f in flds):
                    shape = "(no code reads them)"
                print(("%3d  %4d bytes  %5d uses  %-26s %s" % (cid, size[cid], uses.get(cid, 0), KNOWN.get(cid, ""), shape)).rstrip())
    except SequenceError as e:
        sys.exit("error: %s" % e)


if __name__ == "__main__":
    main()
