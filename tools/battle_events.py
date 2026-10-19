#!/usr/bin/env python3
"""Map the battle's events (decomp/include/battle/event.h): where the battle calls each one, the variables
it sets up first and reads after, and which moves, abilities, items and other things answer it.

  python -P tools/battle_events.py              every event, one line each
  python -P tools/battle_events.py 0x47 ...     those events in full (every call and every handler)

The battle calls an event with battle::CallEvent or CallEventWithSkipChecks, between PushVars and PopVars;
SetVar and the rest set its variables (by label) and GetVar reads them back. Handlers come from tables of
getters (an ID, then a function that writes the handler count and returns the handlers). Names come from
the English message files. Everything printed is derived from the game: read it in the terminal, never
commit or publish it.
"""
import argparse
import bisect
import csv
import os
import struct
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nso2elf  # noqa: E402

BASE = 0x7100000000
BATTLE = (0x71009C0000, 0x7100AB0000)
CALL_EVENT = {0x7100A66B90: "CallEvent", 0x7100A66C70: "CallEventWithSkipChecks"}
PUSH, POP = 0x7100A66FA0, 0x7100A67000
SETTERS = {0x7100A67090: "rewritable", 0x7100A67100: "const", 0x7100A67170: "once", 0x7100A67230: "ratio"}
POINTER_SETTERS = {0x7100A674C0: (0x47, 0x48), 0x7100A67630: (0x23, 0x24)}
GETTERS = {0x7100A673E0: "get", 0x7100A67450: "find", 0x7100A672A0: "rewrite", 0x7100A67340: "multiply"}
POINTER_GETTERS = {0x7100A67590: (0x47, 0x48), 0x7100A67700: (0x23, 0x24)}
MESSAGES = os.path.join(ROOT, "orig", "romfs", "bin", "message", "English", "common")


def message_lines(name):
    """A generation 7 message file's lines (variables left out)."""
    try:
        data = open(os.path.join(MESSAGES, name + ".dat"), "rb").read()
    except OSError:
        return []
    count, section = struct.unpack_from("<H", data, 2)[0], struct.unpack_from("<I", data, 0xC)[0]
    out, key = [], 0x7C89
    for i in range(count):
        offset, length = struct.unpack_from("<iH", data, section + 4 + 8 * i)
        k, units = key, []
        for j in range(length):
            units.append(struct.unpack_from("<H", data, section + offset + 2 * j)[0] ^ k)
            k = ((k << 3) | (k >> 13)) & 0xFFFF
        key = (key + 0x2983) & 0xFFFF
        text, j = [], 0
        while j < len(units) and units[j]:
            if units[j] == 0x10:                    # a variable: its length, then its code and arguments
                j += 2 + units[j + 1]
                continue
            text.append(chr(units[j]) if units[j] < 0xE000 else "?")
            j += 1
        out.append("".join(text))
    return out


class Game:
    def __init__(self):
        nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
        info = nso2elf.analyze(nso)
        self.img = nso["image"]
        self.rel = {r["offset"]: r["addend"] for r in info["relocs"] if r["type"] == 1027}
        rows = sorted((int(r["Address"], 16), int(r["Size"])) for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_functions.csv"))))
        self.funcs = [r for r in rows if BATTLE[0] <= r[0] < BATTLE[1]]
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def u32(self, a):
        return struct.unpack_from("<I", self.img, a - BASE)[0]

    def pointer(self, a):
        o = self.rel.get(a - BASE)
        return BASE + o if o is not None else None

    def code(self, start, size):
        return list(self.md.disasm(self.img[start - BASE:start - BASE + size], start))

    def handlers(self, getter):
        """A getter's handler array: (event, function) pairs. Getters write the count then return the array."""
        count, array, regs = None, None, {}
        for ins in self.code(getter, 48):
            ops = ins.op_str.replace(" ", "").split(",")
            if ins.mnemonic == "mov" and ops[1].startswith("#"):
                regs[ops[0]] = int(ops[1][1:], 0)
            elif ins.mnemonic == "str" and ops[1] == "[x0]" and ops[0] in regs:
                count = regs[ops[0]]
            elif ins.mnemonic == "adrp" and ops[0] == "x0":
                array = int(ops[1][1:], 0)
            elif ins.mnemonic == "add" and ops[0] == "x0" and ops[1] == "x0" and array is not None:
                array += int(ops[2][1:], 0)
            elif ins.mnemonic in ("ret", "b", "bl"):
                break
        if count is None or array is None:
            return []
        return [(self.u32(array + 16 * i), self.pointer(array + 16 * i + 8)) for i in range(count)]


def tables(game):
    """(kind, id, getter) for every handler table the registrars use."""
    out = []
    for kind, table, n in (("move", 0x71015CD300, 0x15F), ("ability", 0x71015CAA10, 0xE2), ("item", 0x71015C8170, 0xBF),
                           ("kind 7", 0x71015C8110, 6)):
        for i in range(n):
            out.append((kind, game.u32(table + 16 * i), game.pointer(table + 16 * i + 8)))
    for kind, table, n in (("position", 0x71015CA5F0, 5), ("side", 0x71015CA6F0, 0x12)):
        for i in range(n):
            out.append((kind, i, game.pointer(table + 16 * i + 8)))
    for i in range(11):                            # the field's go through a table of indices
        index = struct.unpack_from("<q", game.img, 0x7100F97340 - BASE + 8 * i)[0]
        out.append(("field", i, game.pointer(0x71015D1E60 + 16 * index + 8)))
    return [t for t in out if t[2]]


NO_DESTINATION = ("st", "cmp", "cmn", "tst", "b", "cb", "tb", "ret", "nop", "ccmp", "ccmn", "fcmp", "prfm", "brk")


def reg(name):
    """A general register's number (w19 and x19 are 19), 31 for the zero register, None otherwise."""
    if name in ("wzr", "xzr"):
        return 31
    if name[:1] in "wx" and name[1:].isdigit():
        return int(name[1:])
    return None


def calls(game):
    """Each event call: (event or None, function, call address, how it's called, the variables its frame
    set before it, the variables read after it). Follows constants in registers through each function
    in order, which is enough for the battle's straight runs of PushVars, setters, events and PopVars."""
    out = []
    for start, size in game.funcs:
        consts, frame, current = {31: 0}, [], None
        for ins in game.code(start, size):
            ops = ins.op_str.replace(" ", "").split(",")
            if ins.mnemonic == "bl" and ins.op_str.startswith("#"):
                t = int(ins.op_str[1:], 0)
                if t == PUSH:
                    frame, current = [], None
                elif t == POP:
                    frame, current = [], None
                elif t in SETTERS:
                    frame.append((consts.get(0), SETTERS[t]))
                elif t in POINTER_SETTERS:
                    frame += [(l, "pointer") for l in POINTER_SETTERS[t]]
                elif t in CALL_EVENT:
                    current = (consts.get(1, "w1?"), start, ins.address, CALL_EVENT[t], list(frame), [])
                    out.append(current)
                elif t in GETTERS and current is not None:
                    current[5].append((consts.get(0), GETTERS[t]))
                elif t in POINTER_GETTERS and current is not None:
                    current[5].extend((l, "pointer") for l in POINTER_GETTERS[t])
                for r in range(19):                     # a call leaves only x19-x28 as they were
                    consts.pop(r, None)
                consts[31] = 0
                continue
            if ins.mnemonic.startswith(NO_DESTINATION) or not ops or reg(ops[0]) is None:
                continue
            d = reg(ops[0])
            if ins.mnemonic in ("mov", "movz") and len(ops) == 2 and ops[1].startswith("#"):
                consts[d] = int(ops[1][1:], 0)
            elif ins.mnemonic == "mov" and len(ops) == 2 and reg(ops[1]) is not None:
                v = consts.get(reg(ops[1]))
                if v is None:
                    consts.pop(d, None)
                else:
                    consts[d] = v
            else:
                consts.pop(d, None)
                if ins.mnemonic == "ldp" and len(ops) > 1 and reg(ops[1]) is not None:
                    consts.pop(reg(ops[1]), None)
            consts[31] = 0
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events", nargs="*", help="events to show in full")
    a = ap.parse_args()
    game = Game()
    names = {"move": message_lines("wazaname"), "ability": message_lines("tokusei"), "item": message_lines("itemname")}

    def thing(kind, i):
        n = names.get(kind)
        return "%s %s" % (kind, n[i] if n and i < len(n) else i)

    answers = defaultdict(list)
    for kind, i, getter in tables(game):
        for event, fn in game.handlers(getter):
            answers[event].append((kind, i, fn))
    by_event = defaultdict(list)
    for c in calls(game):
        by_event[c[0]].append(c)
    wanted = [int(x, 0) for x in a.events]

    def labels(vs):
        return " ".join("%s:%s" % ("?" if l is None else "0x%x" % l, m) for l, m in vs)

    for event in sorted(set(by_event) | set(answers), key=lambda e: (isinstance(e, str), e if isinstance(e, int) else 0)):
        if wanted and event not in wanted:
            continue
        cs, hs = by_event.get(event, []), answers.get(event, [])
        title = "event 0x%x" % event if isinstance(event, int) else "event %s" % event
        if not wanted:
            setv = sorted({l for c in cs for l, _ in c[4] if l is not None})
            kinds = defaultdict(int)
            for kind, _, _ in hs:
                kinds[kind] += 1
            print("%-12s %2d calls, sets %-34s answered by %s; e.g. %s" % (
                title, len(cs), ",".join("%x" % l for l in setv)[:34] or "-",
                ", ".join("%d %s" % (n, k) for k, n in sorted(kinds.items())) or "nothing",
                ", ".join(thing(k, i) for k, i, _ in hs[:3])))
            continue
        print(title)
        for event_, func, at, how, setv, readv in cs:
            print("  called at 0x%x in 0x%x (%s): sets %s; reads %s" % (at, func, how, labels(setv) or "-", labels(readv) or "-"))
        for kind, i, fn in hs:
            print("  answered by %-40s handler 0x%x" % (thing(kind, i), fn or 0))


if __name__ == "__main__":
    main()
