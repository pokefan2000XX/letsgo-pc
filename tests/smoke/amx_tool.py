"""tools/amx.py on made-up scripts: compact cells, assembling and disassembling back to the same bytes, statements,
strings, publics that move with the code, new strings added to the data, and strings that would cover a public
variable.

  python -P tests/smoke/amx_tool.py      (prints PASS or what failed)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import amx  # noqa: E402

NAMES = {amx.h131("ShowThing_"): {"name": "ShowThing_", "group": "test"},
         amx.h131("Total"): {"name": "Total", "group": "test"}}

SOURCE = r"""
.amx file_version=10 amx_version=10 flags=0x1c defsize=12
.stack 0x2000
.nametable 3f000000
.public GetSceneChangeData
.native ShowThing_
.native Total
.pubvar 0xd7477c97 0x0

.code
    halt.p 0

f_wrap:
    proc
    ShowThing_(arg0)
    retn

main:
    proc
    stack.p -8
    load.p.pri 0
    switch events
first:
    var1 = Total(40, 2)
    f_wrap(var1)
    jump done
second:
    if Total("a, b; c: \"d\"\n", g_0000) goto done
    ShowThing_("brand new", s_0010)
    jump done
events:
    casetbl
        default done
        case 0 first
        case 0x14d1fe7a8c39292d second
done:
    stack.p 8
    zero.pri
    retn

GetSceneChangeData:
    proc
    const.p.alt 128
    load.p.s.pri 24
    lidx.p.b 3
    retn

.data
    .cell 0 0
s_0010:
    .string "a, b; c: \"d\"\n"
    .cell -1 9223372036854775807 -9223372036854775808 64 -65
"""


def check(cond, what):
    if not cond:
        print("FAIL " + what)
        sys.exit(1)


def main():
    # compact cells: pawncc's rule both ways
    cells = [0, 1, -1, 63, 64, -64, -65, 0x7FFFFFFF, -0x80000000, (1 << 63) - 1, -(1 << 63), 0x14D1FE7A8C39292D]
    packed = amx.compress([c & amx.MASK for c in cells])
    check([amx.signed(c) for c in amx.expand(packed)] == cells, "compact cells come back")

    first = amx.assemble(SOURCE, NAMES)
    a = amx.Amx(first)
    check(a.build() == first, "a built file rebuilds the same")
    check(len(a.publics) == 1 and a.publics[0][1] == amx.h131("GetSceneChangeData"), "the public's hash")
    text = amx.disassemble(a, NAMES)
    check(amx.assemble(text, NAMES) == first, "disassembling and assembling gives the same bytes")
    check('Total("a, b; c: \\"d\\"\\n", g_0000)' in text, "a string argument with , ; : and quotes")
    check('    .string "brand new"' in text, "a new string is added to the data")
    check("ShowThing(arg0)" not in text and "ShowThing:" in text, "a one-command wrapper takes its name")
    check("var1 = Total(40, 2)" in text, "an assignment statement")
    check(text.count(".string") == 2, "two strings")

    # code put in before the public moves it: its entry follows the label
    longer = SOURCE.replace("    stack.p -8\n", "    stack.p -8\n    nop\n    nop\n")
    b = amx.Amx(amx.assemble(longer, NAMES))
    check(b.publics[0][0] == a.publics[0][0] + 16, "the public moves with the code")
    check(b.header["cip"] == a.header["cip"], "main stays where it was")

    # a script names the same string twice: the second use is written by label
    twice = SOURCE.replace('ShowThing_("brand new", s_0010)', 'ShowThing_("x", "x")').replace(
        '.cell 0 0\n', '.cell 0 0\ns_dup:\n    .string "x"\n')
    c = amx.assemble(twice, NAMES)
    check(amx.assemble(amx.disassemble(amx.Amx(c), NAMES), NAMES) == c, "repeated strings come back")

    for bad, why in (("    jump nowhere\n", "a missing label"), ('    ShowThing_("open)\n', "an open string")):
        try:
            amx.assemble(SOURCE.replace("    zero.pri\n", bad + "    zero.pri\n", 1), NAMES)
        except amx.SourceError:
            continue
        check(False, why + " is refused")

    # a script with no .data of its own gets its strings at address 0, where the game writes g_mode
    no_data = SOURCE[:SOURCE.index(".data")].replace("s_0010", "\"other\"")
    try:
        amx.assemble(no_data, NAMES)
        check(False, "strings over a public variable are refused")
    except amx.SourceError as e:
        check("public variable" in str(e), "the refusal says why: %s" % e)
    print("PASS")


if __name__ == "__main__":
    main()
