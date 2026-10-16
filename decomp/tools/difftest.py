#!/usr/bin/env python3
"""Check decompiled functions against the originals, both running in the emulator.

  python -P decomp/tools/difftest.py [-k name_part]

Every test_* function in decomp/tests/**/test_*.py runs twice on fresh emulators: once calling
the original functions, once calling their decompiled replacements (build/decomp/patch.elf,
from decomp/tools/build.py). A test feeds inputs and records results with t.check(value);
both runs must record the same values and make the same SDK calls (import handlers, apart from
memcpy/memset and other pure memory functions) and calls to fake functions. Inputs
come from t.rng, seeded the same way in both runs, so random cases repeat exactly.

  def test_name_equals(t):
      obj = t.emu.alloc(0x100)
      t.check(t.call(0x7100101b30, obj, t.emu.cstr("abc")))

Prints PASS or FAIL per test; exits nonzero if any test fails. A test whose functions aren't
decompiled yet is reported and skipped.
"""
import argparse
import glob
import importlib.util
import json
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "emu"))
from guest_emu import GuestEmu  # noqa: E402

PATCH = os.path.join(ROOT, "build", "decomp", "patch.elf")
# Pure memory functions: whether the compiler calls them or inlines them is not a difference
PURE = {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp", "strcpy", "strncpy"}
REPLACES = os.path.join(ROOT, "build", "decomp", "replaces.json")


class Run:
    """What a test sees: the emulator, a seeded RNG, call() and check()."""

    def __init__(self, emu, replaces, use_decomp, seed):
        self.emu = emu
        self.rng = random.Random(seed)
        self.values = []
        self.called = set()            # originals the test called
        self._replaces = replaces
        self._decomp = use_decomp

    def call(self, addr, *args, **kw):
        self.called.add(addr)
        target = self._replaces.get(addr, addr) if self._decomp else addr
        return self.emu.call(target, *args, **kw)

    def check(self, value):
        self.values.append(value)


def run_test(fn, replaces, use_decomp, setup):
    emu = GuestEmu(os.path.join(ROOT, "orig", "exefs", "main"))
    if use_decomp:
        emu.load_patch(PATCH)
    if setup:
        setup(emu)
    t = Run(emu, replaces, use_decomp, seed=fn.__name__)
    fn(t)
    return t, [c for c in emu.calls if c not in PURE]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", default="", help="only tests whose name contains this")
    a = ap.parse_args()
    if not os.path.exists(PATCH):
        print("no decomp module (run decomp/tools/build.py)")
        return 1
    replaces = {int(k, 16): int(v, 16) for k, v in json.load(open(REPLACES)).items()}
    failed = ran = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "decomp", "tests", "**", "test_*.py"), recursive=True)):
        if os.path.dirname(path) not in sys.path:      # helpers next to the tests
            sys.path.insert(0, os.path.dirname(path))
        spec = importlib.util.spec_from_file_location(os.path.basename(path)[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        setup = getattr(mod, "setup", None)        # optional: import handlers etc. for both runs
        for name in sorted(n for n in dir(mod) if n.startswith("test_") and a.k in n):
            fn = getattr(mod, name)
            try:
                orig, orig_calls = run_test(fn, replaces, False, setup)
                if not orig.called or not any(f in replaces for f in orig.called):
                    print("SKIP  %s: %s not decompiled" % (name, ", ".join("0x%x" % f for f in sorted(orig.called))))
                    continue
                new, new_calls = run_test(fn, replaces, True, setup)
            except Exception as e:                  # a fault or an unhandled import
                print("FAIL  %s: %s" % (name, e))
                failed += 1
                continue
            ran += 1
            if orig.values != new.values:
                k = next(i for i in range(max(len(orig.values), len(new.values)))
                         if i >= len(orig.values) or i >= len(new.values) or orig.values[i] != new.values[i])
                print("FAIL  %s: check #%d: original %r, decompiled %r" % (
                    name, k, orig.values[k] if k < len(orig.values) else "(none)",
                    new.values[k] if k < len(new.values) else "(none)"))
                failed += 1
            elif orig_calls != new_calls:
                print("FAIL  %s: SDK calls differ: original %s, decompiled %s" % (name, orig_calls[:20], new_calls[:20]))
                failed += 1
            else:
                print("PASS  %s (%d checks, %d SDK calls)" % (name, len(orig.values), len(orig_calls)))
    print("%d tests, %d failed" % (ran, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
