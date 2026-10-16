#!/usr/bin/env python3
"""Dump main.npdm: main-thread setup, memory/address-space flags, the
services the game may open (SAC), and the kernel capabilities (allowed SVCs).

Usage: python -P tools/npdm.py orig/exefs/main.npdm [--json data/main_npdm.json]
"""
import argparse
import json
import struct
import sys


def parse_sac(b):
    out, o = [], 0
    while o < len(b):
        ctrl = b[o]
        n = (ctrl & 7) + 1
        out.append(("server:" if ctrl & 0x80 else "") + b[o + 1:o + 1 + n].decode())
        o += 1 + n
    return out


def parse_kac(b):
    caps = {"svcs": [], "other": []}
    for (d,) in struct.iter_unpack("<I", b):
        if d == 0xFFFFFFFF:
            continue
        low = (~d & (d + 1)).bit_length() - 1  # number of trailing ones
        if low == 3:
            caps["thread_info"] = {"lowest_prio": (d >> 4) & 0x3F, "highest_prio": (d >> 10) & 0x3F,
                                   "min_core": (d >> 16) & 0xFF, "max_core": (d >> 24) & 0xFF}
        elif low == 4:
            mask, idx = (d >> 5) & 0xFFFFFF, (d >> 29) & 7
            caps["svcs"] += [idx * 24 + i for i in range(24) if mask >> i & 1]
        elif low == 13:
            caps["program_type"] = (d >> 14) & 7
        elif low == 14:
            caps["kernel_version"] = "%d.%d" % ((d >> 19) & 0x1FFF, (d >> 15) & 0xF)
        elif low == 15:
            caps["handle_table_size"] = (d >> 16) & 0x3FF
        elif low == 16:
            caps["debug_flags"] = (d >> 17) & 3
        else:
            caps["other"].append("0x%08x" % d)
    caps["svcs"] = sorted(caps["svcs"])
    return caps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npdm")
    ap.add_argument("--json")
    a = ap.parse_args()
    b = open(a.npdm, "rb").read()
    if b[:4] != b"META":
        raise ValueError("not an NPDM")
    flags = b[0xC]
    info = {
        "is_64bit": bool(flags & 1), "address_space_type": (flags >> 1) & 7,
        "main_thread_priority": b[0xE], "main_thread_core": b[0xF],
        "system_resource_size": struct.unpack_from("<I", b, 0x14)[0],
        "version": struct.unpack_from("<I", b, 0x18)[0],
        "main_thread_stack_size": struct.unpack_from("<I", b, 0x1C)[0],
        "name": b[0x20:0x30].split(b"\0")[0].decode(),
        "product_code": b[0x30:0x40].split(b"\0")[0].decode(),
    }
    aci_off, aci_size = struct.unpack_from("<II", b, 0x70)
    aci = b[aci_off:aci_off + aci_size]
    if aci[:4] == b"ACI0":
        info["program_id"] = "%016x" % struct.unpack_from("<Q", aci, 0x10)[0]
        sac_off, sac_size = struct.unpack_from("<II", aci, 0x28)
        kac_off, kac_size = struct.unpack_from("<II", aci, 0x30)
        info["services"] = parse_sac(aci[sac_off:sac_off + sac_size])
        info["kernel_caps"] = parse_kac(aci[kac_off:kac_off + kac_size])
    if a.json:
        json.dump(info, open(a.json, "w"), indent=1)
    print(json.dumps(info, indent=1))


if __name__ == "__main__":
    sys.exit(main())
