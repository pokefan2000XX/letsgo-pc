#!/usr/bin/env python3
"""How much of the game is decompiled: overall and per roadmap phase (docs/PLAN.md).

  python -P decomp/tools/progress.py

Counts the functions of main that a decompiled function replaces (build/decomp/replaces.json,
from decomp/tools/build.py) against data/main_functions.csv, by count and by code size.
"""
import csv
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Roadmap phases and the parts of main they cover (filled in as each phase's code is found)
PHASES = [
    ("D1 Pokemon data", [(0x710071C000, 0x710073A400)]),
    ("D2 Saves", []),
    ("D3 Script commands", []),
    ("D4 Game data tables", []),
    ("D5 Battle", []),
    ("D6 Field and catching", []),
    ("D7 Menus and screens", []),
]


def main():
    funcs = {int(r["Address"], 16): int(r["Size"]) for r in csv.DictReader(
        open(os.path.join(ROOT, "data", "main_functions.csv"))) if r["Source"] != "plt"}
    path = os.path.join(ROOT, "build", "decomp", "replaces.json")
    done = {int(k, 16) for k in json.load(open(path))} if os.path.exists(path) else set()
    done &= set(funcs)

    def line(label, addrs):
        n, size = len(addrs), sum(funcs[a] for a in addrs)
        d = [a for a in addrs if a in done]
        dsize = sum(funcs[a] for a in d)
        return "%-24s %6d of %6d functions (%5.1f%%)  %8d of %8d bytes (%5.1f%%)" % (
            label, len(d), n, 100 * len(d) / n if n else 0, dsize, size, 100 * dsize / size if size else 0)

    print(line("main", sorted(funcs)))
    for name, ranges in PHASES:
        if ranges:
            print(line(name, [a for a in funcs if any(lo <= a < hi for lo, hi in ranges)]))
        else:
            print("%-24s (not mapped yet)" % name)


if __name__ == "__main__":
    main()
