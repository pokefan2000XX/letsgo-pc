"""Let's Go's message files (generation 7's format) to text and back (docs/PLAN.md, roadmap phase 6, step 1).

A message file (bin/message/<language>/{common,script}/<name>.dat) holds a list of lines; its label table (<name>.tbl,
"AHTB") names them. Each line is UTF-16 with codes inside it, encrypted with a key that depends on its index, and has a
16-bit attribute. The text form has one line per message:

    label<TAB>text            or      label@attribute<TAB>text      (a nonzero attribute, decimal)

In the text: \\n a new line, \\r scroll the box, \\c clear it, \\\\ and \\[ a backslash and a bracket, [WAIT n] a pause,
[~ n] an empty line, [VAR code(arg,...)] a variable (the player's name, a number, a colour: hex code and arguments),
\\x{hhhh} any other unit outside plain text, and [RAW hhhh ...] what can't be said another way. Files without a label
table get labels #0, #1, ... A label table also names the end of the list (msg_<name>_max): the last row,
#end<TAB>label, keeps it.

  python -P tools/msg.py dump <file.dat> [out.txt]       the text (to stdout without out.txt)
  python -P tools/msg.py build <in.txt> <out.dat>        a message file, and <out>.tbl when the lines have labels
  python -P tools/msg.py extract [outdir]                every English file as text (default build/messages)
  python -P tools/msg.py roundtrip [files...]            dump and build each file (default: every language's)
                                                         and check the bytes come back the same
"""
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESSAGES = os.path.join(ROOT, "orig", "romfs", "bin", "message")

KEY_BASE, KEY_STEP = 0x7C89, 0x2983
VARIABLE = 0x0010
RETURN, CLEAR, WAIT, NULL = 0xBE00, 0xBE01, 0xBE02, 0xBDFF


class MessageError(Exception):
    pass


def fnv1a64(data):
    h = 0xCBF29CE484222645
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def crypt(units, index):
    """Encrypts or decrypts one line's units (the same operation)."""
    key = (KEY_BASE + index * KEY_STEP) & 0xFFFF
    out = []
    for u in units:
        out.append(u ^ key)
        key = ((key << 3) | (key >> 13)) & 0xFFFF
    return out


# ---- the binary files ---------------------------------------------------------------------------

def read_dat(data):
    """[(attribute, units)] of a message file; each line's units end with its terminator."""
    if len(data) < 0x10:
        raise MessageError("too short for a message file")
    sections, count, total, key, offset = struct.unpack_from("<HHIII", data, 0)
    if sections != 1 or key != 0:
        raise MessageError("not a one-section message file")
    lines = []
    for i in range(count):
        start, length, attribute = struct.unpack_from("<iHH", data, offset + 4 + 8 * i)
        raw = struct.unpack_from("<%dH" % length, data, offset + start)
        lines.append((attribute, crypt(raw, i)))
    return lines


def write_dat(lines):
    table, text = [], bytearray()
    base = 4 + 8 * len(lines)
    for i, (attribute, units) in enumerate(lines):
        table.append(struct.pack("<iHH", base + len(text), len(units), attribute))
        text += struct.pack("<%dH" % len(units), *crypt(units, i))
        if len(text) % 4:
            text += b"\0\0"
    section = struct.pack("<I", base + len(text)) + b"".join(table) + bytes(text)
    return struct.pack("<HHIII", 1, len(lines), len(section), 0, 0x10) + section


def read_tbl(data):
    if data[:4] != b"AHTB":
        raise MessageError("not a label table")
    count, = struct.unpack_from("<I", data, 4)
    labels, pos = [], 8
    for _ in range(count):
        h, length = struct.unpack_from("<QH", data, pos)
        name = data[pos + 10:pos + 10 + length].rstrip(b"\0").decode("utf-8")
        if fnv1a64(name.encode("utf-8")) != h:
            raise MessageError("label %r doesn't match its hash" % name)
        labels.append(name)
        pos += 10 + length
    return labels


def write_tbl(labels):
    out = bytearray(b"AHTB" + struct.pack("<I", len(labels)))
    for name in labels:
        b = name.encode("utf-8") + b"\0"
        out += struct.pack("<QH", fnv1a64(b[:-1]), len(b)) + b
    return bytes(out)


# ---- units to text and back ---------------------------------------------------------------------

def plain(u):
    """A unit that stands for itself in the text."""
    if u in (0x5C, 0x5B):           # backslash, [
        return False
    if u < 0x20 or 0x7F <= u < 0xA0 or 0xD800 <= u < 0xF900 or u >= 0xFFF0:
        return False
    return True


def units_to_text(units):
    """One line's units (with its terminator) as text."""
    if not units or units[-1] != 0:
        body, end = list(units), "[NOEND]"
    else:
        body, end = list(units[:-1]), ""
    out, i = [], 0
    while i < len(body):
        u = body[i]
        if u == VARIABLE and i + 1 < len(body) and body[i + 1] >= 1 and i + 2 + body[i + 1] <= len(body):
            count = body[i + 1]
            code, args = body[i + 2], body[i + 3:i + 2 + count]
            i += 2 + count
            if code == RETURN and not args:
                out.append("\\r")
            elif code == CLEAR and not args:
                out.append("\\c")
            elif code == WAIT and len(args) == 1:
                out.append("[WAIT %d]" % args[0])
            elif code == NULL and len(args) == 1:
                out.append("[~ %d]" % args[0])
            elif args:
                out.append("[VAR %04X(%s)]" % (code, ",".join("%04X" % a for a in args)))
            else:
                out.append("[VAR %04X]" % code)
            continue
        if u == VARIABLE:
            out.append("[RAW %s]" % " ".join("%04X" % x for x in body[i:]))
            break
        if u == 0x0A:
            out.append("\\n")
        elif u == 0x5C:
            out.append("\\\\")
        elif u == 0x5B:
            out.append("\\[")
        elif plain(u):
            out.append(chr(u))
        else:
            out.append("\\x{%04X}" % u)
        i += 1
    return "".join(out) + end


TOKEN = re.compile(r"\\n|\\r|\\c|\\\\|\\\[|\\x\{([0-9A-Fa-f]{4})\}|\[WAIT (\d+)\]|\[~ (\d+)\]|"
                   r"\[VAR ([0-9A-Fa-f]{4})(?:\(([0-9A-Fa-f]{4}(?:,[0-9A-Fa-f]{4})*)\))?\]|"
                   r"\[RAW ((?:[0-9A-Fa-f]{4} ?)+)\]|\[NOEND\]$")


def text_to_units(text):
    """The units of one line of text, with its terminator unless it ends in [NOEND]."""
    units, pos, end = [], 0, True
    while pos < len(text):
        c = text[pos]
        if c not in "\\[":
            units.append(ord(c))
            pos += 1
            continue
        m = TOKEN.match(text, pos)
        if not m:
            raise MessageError("can't read %r" % text[pos:pos + 24])
        t = m.group(0)
        if t == "\\n":
            units.append(0x0A)
        elif t == "\\r":
            units += [VARIABLE, 1, RETURN]
        elif t == "\\c":
            units += [VARIABLE, 1, CLEAR]
        elif t == "\\\\":
            units.append(0x5C)
        elif t == "\\[":
            units.append(0x5B)
        elif m.group(1):
            units.append(int(m.group(1), 16))
        elif m.group(2):
            units += [VARIABLE, 2, WAIT, int(m.group(2))]
        elif m.group(3):
            units += [VARIABLE, 2, NULL, int(m.group(3))]
        elif m.group(4):
            args = [int(a, 16) for a in m.group(5).split(",")] if m.group(5) else []
            units += [VARIABLE, 1 + len(args), int(m.group(4), 16)] + args
        elif m.group(6):
            units += [int(a, 16) for a in m.group(6).split()]
        elif t == "[NOEND]":
            end = False
        pos = m.end()
    if any(u > 0xFFFF for u in units):
        raise MessageError("a character outside UTF-16's first plane")
    return units + [0] if end else units


# ---- files ----------------------------------------------------------------------------------------

LABEL = re.compile(r"^([^\t@]+)(?:@(\d+))?\t(.*)$")


def dump(dat_path):
    lines = read_dat(open(dat_path, "rb").read())
    tbl_path = os.path.splitext(dat_path)[0] + ".tbl"
    labels = read_tbl(open(tbl_path, "rb").read()) if os.path.exists(tbl_path) else None
    if labels is not None and len(labels) != len(lines) + 1:
        raise MessageError("%d labels for %d lines" % (len(labels), len(lines)))
    out = ["# %s: %d lines. Edit the text after the tab; tools/msg.py build writes the file back."
           % (os.path.basename(dat_path), len(lines))]
    for i, (attribute, units) in enumerate(lines):
        label = labels[i] if labels is not None else "#%d" % i
        out.append("%s%s\t%s" % (label, "@%d" % attribute if attribute else "", units_to_text(units)))
    if labels is not None:
        out.append("#end\t%s" % labels[-1])
    return "\n".join(out) + "\n"


def parse(text):
    """([(label, attribute, units)], the end label or None) from the text form."""
    lines, end = [], None
    for n, row in enumerate(text.split("\n"), 1):
        if not row or row.startswith("# "):
            continue
        if row.startswith("#end\t"):
            end = row[5:].rstrip("\r")
            continue
        m = LABEL.match(row.rstrip("\r"))
        if not m:
            raise MessageError("line %d: expected label<TAB>text" % n)
        try:
            lines.append((m.group(1), int(m.group(2) or 0), text_to_units(m.group(3))))
        except MessageError as e:
            raise MessageError("line %d (%s): %s" % (n, m.group(1), e))
    return lines, end


def build(text):
    """(dat bytes, tbl bytes or None) from the text form."""
    lines, end = parse(text)
    labels = [label for label, _, _ in lines]
    if len(set(labels)) != len(labels):
        raise MessageError("two lines have the same label")
    if end is None and not all(re.fullmatch(r"#\d+", label) for label in labels):
        raise MessageError("labelled lines need the #end row (the label table's last name)")
    dat = write_dat([(attribute, units) for _, attribute, units in lines])
    return dat, None if end is None else write_tbl(labels + [end])


def roundtrip(paths):
    bad = 0
    for p in paths:
        try:
            dat, tbl = build(dump(p))
            ok = dat == open(p, "rb").read()
            tbl_path = os.path.splitext(p)[0] + ".tbl"
            if tbl is not None or os.path.exists(tbl_path):
                ok = ok and tbl == open(tbl_path, "rb").read()
        except MessageError as e:
            ok = False
            print("%s: %s" % (os.path.relpath(p, ROOT), e))
        if not ok:
            bad += 1
            print("differs:", os.path.relpath(p, ROOT))
    print("%d of %d message files come back byte for byte" % (len(paths) - bad, len(paths)))
    return bad == 0


def all_files(language=None):
    """The message files (not grammar/, which holds other data)."""
    pattern = os.path.join(MESSAGES, language or "*", "**", "*.dat")
    return sorted(p for p in glob.glob(pattern, recursive=True)
                  if os.path.relpath(p, MESSAGES).split(os.sep)[0] != "grammar")


def write_out(text):
    sys.stdout.buffer.write(text.encode("utf-8"))


def main(argv):
    if len(argv) < 2 or argv[1] not in ("dump", "build", "extract", "roundtrip"):
        write_out(__doc__)
        return 2
    try:
        if argv[1] == "dump" and len(argv) in (3, 4):
            text = dump(argv[2])
            if len(argv) == 4:
                open(argv[3], "w", encoding="utf-8", newline="\n").write(text)
            else:
                write_out(text)
        elif argv[1] == "build" and len(argv) == 4:
            dat, tbl = build(open(argv[2], encoding="utf-8").read())
            open(argv[3], "wb").write(dat)
            if tbl is not None:
                open(os.path.splitext(argv[3])[0] + ".tbl", "wb").write(tbl)
        elif argv[1] == "extract":
            out = argv[2] if len(argv) > 2 else os.path.join(ROOT, "build", "messages")
            paths = all_files("English")
            for p in paths:
                rel = os.path.relpath(p, os.path.join(MESSAGES, "English"))
                target = os.path.join(out, os.path.splitext(rel)[0] + ".txt")
                os.makedirs(os.path.dirname(target), exist_ok=True)
                open(target, "w", encoding="utf-8", newline="\n").write(dump(p))
            print("wrote %d files to %s" % (len(paths), out))
        elif argv[1] == "roundtrip":
            return 0 if roundtrip(argv[2:] or all_files()) else 1
        else:
            write_out(__doc__)
            return 2
    except (MessageError, OSError) as e:
        print("msg.py: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
