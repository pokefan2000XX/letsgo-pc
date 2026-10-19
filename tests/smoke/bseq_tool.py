"""tools/bseq.py on a made-up battle sequence and size table: read and built byte for byte, through the JSON, its values
typed as the game's code reads each command (texts, ints, floats, [x, y, z], true or false), a value its type can't show
kept as raw ints, commands out of order sorted, and the mistakes it refuses.

  python -P tests/smoke/bseq_tool.py      (prints PASS or what failed)
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import bseq  # noqa: E402

# made-up sizes: the commands used here as their signatures add up, 11 (which no code reads) and the rest 8
SIZES = [8] * bseq.COUNT
SIZES[0] = 0
for cid in (54, 62, 112, 115, 178):
    SIZES[cid] = sum(int(t[1:]) if t[0] == "s" else bseq.KINDS[t] for t in bseq.SIGNATURES[cid].split())
SIZES[11] = 8
HASH = 0xcbf29ce484222645


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def refuses(what, f, *args):
    try:
        f(*args)
    except bseq.SequenceError:
        return
    print("FAIL accepted " + what)
    sys.exit(1)


def text(s, size):
    return s.encode("ascii") + bytes(size - len(s))


def command(start, end, a, b, cid, params):
    return struct.pack("<iiHHHH", start, end, a, b, 0, cid) + params


def made_up(particle="data:/bin/battle/waza/particle/ew999/ew999_made_up.ptcl", scale=(1.5, -250.0, 0.3)):
    body = b"".join([
        command(0, 0, 0, 0, 115, text("Waza_ew9999", 64)),
        command(10, 40, 7, 0, 0, b""),
        command(20, 20, 0, 110, 62, struct.pack("<3fIi", *scale, 1, 7)),
        command(30, 30, 0, 0, 112, text("Play_SE_made_up", 64) + struct.pack("<I", 1)),
        command(31, 60, 10, 0, 54, text(particle, 256) + struct.pack("<5i", 1, 1, 0, 6, 0)),
        command(32, 32, 0, 0, 11, struct.pack("<ii", 5, -1)),
        command(33, 33, 0, 0, 178, struct.pack("<QiI", HASH, 3, 0)),
    ])
    return struct.pack("<4sIII", b"SESD", 3, 0, 90) + body + struct.pack("<i", -1)


def main():
    data = made_up()
    seq = bseq.parse(data, SIZES)
    check(seq["frames"] == 90 and len(seq["commands"]) == 7, "the header and the commands")
    check([c["command"] for c in seq["commands"]] == [115, 0, 62, 112, 54, 11, 178], "the commands' numbers")
    check(bseq.build(seq, SIZES) == data, "built back byte for byte")

    # the JSON: each value as the game reads it, floats as written (0.3, not 0.30000001192092896)
    obj = json.loads(bseq.dumps(bseq.to_json(seq, SIZES)))
    params = [c.get("params") for c in obj["commands"]]
    check(params[0] == ["Waza_ew9999"], "a sequence name")
    check(params[1] == [], "a command without parameters")
    check(params[2] == [[1.5, -250.0, 0.3], True, 7], "[x, y, z], true or false and an int")
    check(params[3] == ["Play_SE_made_up", True], "a sound")
    check(params[4] == ["data:/bin/battle/waza/particle/ew999/ew999_made_up.ptcl", 1, True, 0, 6, 0], "a particle file")
    check(params[5] == [5, -1], "a command no code reads, as ints")
    check(params[6] == [HASH, 3, False], "a 64-bit value")
    check(bseq.build(bseq.from_json(obj, SIZES), SIZES) == data, "the JSON built back byte for byte")

    # values changed through the JSON: the same as a file made with them; a whole number in a float is that float
    obj["commands"][4]["params"][0] = "data:/bin/battle/waza/particle/ew053/ew053_fire_hit.ptcl"
    obj["commands"][2]["params"][0] = [2, 0, 0.5]
    check(bseq.build(bseq.from_json(obj, SIZES), SIZES) == made_up("data:/bin/battle/waza/particle/ew053/ew053_fire_hit.ptcl",
                                                                   (2.0, 0.0, 0.5)), "values changed")

    # a float that isn't a number can't be shown as one: that command's bytes are kept as raw ints
    odd = bseq.parse(made_up(scale=(float("nan"), 1.0, 1.0)), SIZES)
    odd_json = json.loads(bseq.dumps(bseq.to_json(odd, SIZES)))
    check("raw" in odd_json["commands"][2] and "params" not in odd_json["commands"][2], "raw ints for a value its type can't show")
    check(bseq.build(bseq.from_json(odd_json, SIZES), SIZES) == bseq.build(odd, SIZES), "raw ints built back byte for byte")

    # commands out of order are sorted by start frame, the same starts keeping their order
    shuffled = dict(seq, commands=[seq["commands"][4]] + seq["commands"][:4] + seq["commands"][5:])
    check(bseq.build(shuffled, SIZES) == data, "sorted by start frame")

    refuses("a file that isn't a sequence", bseq.parse, b"SESX" + data[4:], SIZES)
    refuses("version 2", bseq.parse, data[:4] + struct.pack("<I", 2) + data[8:], SIZES)
    refuses("no -1 after the last command", bseq.parse, data[:-4], SIZES)
    refuses("bytes after the commands", bseq.parse, data + bytes(4), SIZES)
    refuses("a command the table hasn't", bseq.parse, data, SIZES[:100])

    def changed(k, v, key="params"):
        o = json.loads(bseq.dumps(bseq.to_json(seq, SIZES)))
        o["commands"][k][key] = v
        return o
    refuses("a text longer than its field", bseq.from_json, changed(3, ["x" * 64, True]), SIZES)
    refuses("2 for true or false", bseq.from_json, changed(3, ["Play_SE_made_up", 2]), SIZES)
    refuses("a text where a float goes", bseq.from_json, changed(2, [["1", 0, 0], True, 7]), SIZES)
    refuses("a missing value", bseq.from_json, changed(2, [[1.0, 0.0, 0.0], True]), SIZES)
    refuses("a float where an int goes", bseq.from_json, changed(5, [5.5, -1]), SIZES)
    refuses("a command the game hasn't", bseq.from_json, changed(5, 300, "command"), SIZES)
    bad_frames = dict(seq, commands=[dict(seq["commands"][2], start=9, end=3)])
    refuses("an end before the start", bseq.build, bad_frames, SIZES)
    wrong_size = dict(seq, commands=[dict(seq["commands"][2], params=b"\0" * 4)])
    refuses("parameters of the wrong size", bseq.build, wrong_size, SIZES)
    print("PASS")


if __name__ == "__main__":
    main()
