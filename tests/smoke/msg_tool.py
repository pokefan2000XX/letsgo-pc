"""tools/msg.py on made-up messages: the line encryption, every code of the text form, building a message file and its label
table and dumping them back to the same text and bytes, unlabelled files, and the mistakes it refuses.

  python -P tests/smoke/msg_tool.py      (prints PASS or what failed)
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import msg  # noqa: E402

# Every form the text can take: breaks, scrolling, clearing, waits, empty lines, variables with and without arguments,
# escaped backslashes and brackets, a unit outside plain text, a broken code kept as raw units (only ever at a line's
# end, where it takes the rest), an attribute and a line with no terminator.
TEXT = "\n".join([
    "# made_up.dat: 6 lines. Edit the text after the tab; tools/msg.py build writes the file back.",
    "msg_made_up_01\tHello, [VAR 0100(0000)]!\\nWelcome to the made-up town.",
    "msg_made_up_02\tFirst page.\\rSecond page.\\cA new box.[WAIT 30] Done.",
    "msg_made_up_03@3\tA line with attribute 3 and [~ 2] an empty line.",
    "msg_made_up_04\tA back\\\\slash, a \\[bracket], a private unit \\x{E000} and [VAR BE05].",
    "msg_made_up_05\tPicked by gender: [VAR 1100(00FF,0403)]boygirl, then a broken code[RAW 0010 0009 0041]",
    "msg_made_up_06\tNo terminator here.[NOEND]",
    "#end\tmsg_made_up_max",
]) + "\n"


def check(condition, what):
    if not condition:
        raise AssertionError(what)


def expect_error(text, what):
    try:
        msg.build(text)
    except msg.MessageError:
        return
    raise AssertionError("not refused: " + what)


def main():
    # The encryption is its own inverse, and each line's key starts from its index.
    units = [ord(c) for c in "Made-up"] + [0]
    for index in (0, 1, 77):
        check(msg.crypt(msg.crypt(units, index), index) == units, "crypt twice gives the units back (line %d)" % index)
    check(msg.crypt([0], 0) == [0x7C89] and msg.crypt([0], 1) == [(0x7C89 + 0x2983) & 0xFFFF], "the key of lines 0 and 1")

    # Each code becomes its units and back.
    check(msg.text_to_units("A\\nB") == [0x41, 0x0A, 0x42, 0], "a new line")
    check(msg.text_to_units("\\r\\c") == [0x10, 1, 0xBE00, 0x10, 1, 0xBE01, 0], "scroll and clear")
    check(msg.text_to_units("[WAIT 30][~ 2]") == [0x10, 2, 0xBE02, 30, 0x10, 2, 0xBDFF, 2, 0], "a wait and an empty line")
    check(msg.text_to_units("[VAR 0100(0000)]") == [0x10, 2, 0x0100, 0, 0], "a variable with an argument")
    check(msg.text_to_units("x[NOEND]") == [0x78], "no terminator")
    for line in TEXT.split("\n")[1:7]:
        body = line.split("\t", 1)[1]
        check(msg.units_to_text(msg.text_to_units(body)) == body, "text back from units: " + body)

    # A file and its labels: built, read back, dumped to the same text and built again to the same bytes.
    dat, tbl = msg.build(TEXT)
    lines = msg.read_dat(dat)
    check(len(lines) == 6, "six lines")
    check(lines[2][0] == 3 and lines[0][0] == 0, "the attribute is kept")
    labels = msg.read_tbl(tbl)
    check(labels == ["msg_made_up_0%d" % i for i in range(1, 7)] + ["msg_made_up_max"], "the labels, then the end of the list")
    check(tbl[:4] == b"AHTB" and int.from_bytes(tbl[8:16], "little") == msg.fnv1a64(b"msg_made_up_01"), "labels by their FNV-1a 64")
    folder = tempfile.mkdtemp(prefix="msg_tool_")
    path = os.path.join(folder, "made_up.dat")
    with open(path, "wb") as f:
        f.write(dat)
    with open(os.path.splitext(path)[0] + ".tbl", "wb") as f:
        f.write(tbl)
    dumped = msg.dump(path)
    check(dumped == TEXT, "the dump is the text it was built from:\n" + dumped)
    check(msg.build(dumped) == (dat, tbl), "built again, the same bytes")
    check(msg.roundtrip([path]), "roundtrip says so too")

    # A file without labels: lines #0, #1 and no table.
    plain_dat, plain_tbl = msg.build("#0\tOne.\n#1\tTwo.\n")
    check(plain_tbl is None and len(msg.read_dat(plain_dat)) == 2, "unlabelled lines need no table")

    # Mistakes are refused.
    expect_error("msg_a\tOne.\nmsg_a\tTwo.\n#end\tmsg_max\n", "two lines with one label")
    expect_error("msg_a\tOne.\n", "labelled lines without the #end row")
    expect_error("msg_a One.\n#end\tmsg_max\n", "a row without a tab")
    expect_error("msg_a\tAn [UNKNOWN] code.\n#end\tmsg_max\n", "a bracket that isn't a code")
    try:
        msg.read_tbl(b"AHTB" + (1).to_bytes(4, "little") + (12345).to_bytes(8, "little") + (3).to_bytes(2, "little") + b"ab\0")
        raise AssertionError("not refused: a label that doesn't match its hash")
    except msg.MessageError:
        pass
    print("PASS")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
