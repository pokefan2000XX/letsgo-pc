#!/usr/bin/env python3
"""Assemble the AArch64 guest sysroot for compiling decomp code:

  <out>/include/               musl headers for aarch64 (bits/ from generic + aarch64,
                               bits/alltypes.h and bits/syscall.h generated like musl's Makefile)
  <out>/include/c++/config/    a guest __config_site for llvm-mingw's libc++ headers
                               (ABI v1 / std::__1 like the game's libc++, musl, pthreads,
                               no hardening)

The musl tarball is only read; its build scripts are not run (the two
generated headers are reproduced in Python).

Usage: python -P tools/make_guest_sysroot.py --musl toolchain/src/musl-1.2.6 --out toolchain/guest-sysroot
"""
import argparse
import os
import re
import shutil
import sys

CONFIG_SITE = """// Guest (AArch64, Switch) configuration for libc++ headers. Mirrors the
// game's own libc++: ABI version 1 (std::__1), musl-based libc, pthreads.
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_MUSL_LIBC 1
#define _LIBCPP_HAS_THREAD_API_PTHREAD 1
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 0
#define _LIBCPP_HAS_RANDOM_DEVICE 0
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_PSTL_BACKEND_SERIAL

// No hardening checks: the original has none.
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2

#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0
#define _LIBCPP_LIBC_LLVM_LIBC 0

#endif // _LIBCPP___CONFIG_SITE
"""


def mkalltypes(arch_in, generic_in):
    """Python version of musl's tools/mkalltypes.sed."""
    out = []
    for line in (arch_in + generic_in).splitlines():
        m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
        if m:
            t, n = m.groups()
            out += ["#if defined(__NEED_%s) && !defined(__DEFINED_%s)" % (n, n), "typedef %s %s;" % (t, n),
                    "#define __DEFINED_%s" % n, "#endif", ""]
            continue
        m = re.match(r"^(STRUCT|UNION) *([^ ]*) (.*);$", line)
        if m:
            kind, n, body = m.groups()
            k = kind.lower()
            out += ["#if defined(__NEED_%s_%s) && !defined(__DEFINED_%s_%s)" % (k, n, k, n),
                    "%s %s %s;" % (k, n, body), "#define __DEFINED_%s_%s" % (k, n), "#endif", ""]
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--musl", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    src, out = a.musl, a.out
    inc = os.path.join(out, "include")
    if os.path.exists(inc):
        shutil.rmtree(inc)
    # public headers
    for root, _, files in os.walk(os.path.join(src, "include")):
        rel = os.path.relpath(root, os.path.join(src, "include"))
        for f in files:
            if f.endswith(".h"):
                os.makedirs(os.path.join(inc, rel), exist_ok=True)
                shutil.copy2(os.path.join(root, f), os.path.join(inc, rel, f))
    # bits: generic first, aarch64 overrides
    bits = os.path.join(inc, "bits")
    os.makedirs(bits, exist_ok=True)
    for arch in ("generic", "aarch64"):
        d = os.path.join(src, "arch", arch, "bits")
        for f in os.listdir(d):
            if f.endswith(".h"):
                shutil.copy2(os.path.join(d, f), os.path.join(bits, f))
    read = lambda *p: open(os.path.join(src, *p), encoding="utf-8").read()
    with open(os.path.join(bits, "alltypes.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(mkalltypes(read("arch", "aarch64", "bits", "alltypes.h.in"), read("include", "alltypes.h.in")))
    sc = read("arch", "aarch64", "bits", "syscall.h.in")
    with open(os.path.join(bits, "syscall.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(sc + "".join(l.replace("__NR_", "SYS_", 1) + "\n" for l in sc.splitlines() if "__NR_" in l))
    # libc++ guest config
    cfg = os.path.join(out, "include", "c++", "config")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "__config_site"), "w", encoding="utf-8", newline="\n") as f:
        f.write(CONFIG_SITE)
    with open(os.path.join(out, "VERSION.txt"), "w") as f:
        f.write("musl %s (aarch64 headers) + libc++ guest __config_site\n" % read("VERSION").strip())
    n = sum(len(fs) for _, _, fs in os.walk(inc))
    print("guest sysroot: %d headers in %s" % (n, inc))


if __name__ == "__main__":
    sys.exit(main())
