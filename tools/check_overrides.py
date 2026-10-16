"""Check that every override name registered in runtime/src matches a symbol.

The loader only warns at run time about override names that bind to nothing;
this catches mangling typos before a build. Usage: python -P tools/check_overrides.py
"""
import csv
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    names = {}
    for f in sorted(glob.glob(os.path.join(ROOT, "runtime", "src", "*.cpp"))):
        for m in re.finditer(r'"((?:_Z|nn|nv)[A-Za-z0-9_]+)"', open(f, encoding="utf-8").read()):
            names.setdefault(m.group(1), os.path.basename(f))
    syms = set()
    for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_imports.csv"), encoding="utf-8")):
        syms.add(r["Name"])
    for mod in ("main", "sdk"):
        for line in open(os.path.join(ROOT, "data", mod + "_functions.csv"), encoding="utf-8"):
            parts = line.split(",")
            if len(parts) > 2:
                syms.add(parts[2].split("@")[0])
    bad = sorted((f, n) for n, f in names.items() if n not in syms and not n.startswith(("nvn", "nv::")))
    for f, n in bad:
        print("%s: no symbol %s" % (f, n))
    print("%d names checked, %d unmatched" % (len(names), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
