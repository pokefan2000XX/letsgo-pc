#!/usr/bin/env python3
"""Let's Go's Pawn scripts (AMX): read, disassemble into editable source, assemble back.

  python -P tools/amx.py info <file.amx>                 header and tables
  python -P tools/amx.py disasm <file.amx> [-o out.pasm]  editable source (stdout without -o)
  python -P tools/amx.py asm <file.pasm> -o <file.amx>    back to an AMX the game loads
  python -P tools/amx.py extract [outdir]                 the field scripts, named, from orig/romfs
  python -P tools/amx.py roundtrip [file|dir ...]         disassemble and assemble every script, compare bytes
  python -P tools/amx.py commands [--uses]                the script commands (names, groups; --uses: calls and
                                                         argument counts in the game's scripts)
  python -P tools/amx.py commands --ai                    the battle AI's AI_CMD subcommands (docs/BATTLE_AI.md)

The format (docs/SCRIPTS.md has it in full): Pawn 3.3's AMX with 64-bit cells, compact encoding (each cell as big-endian
7-bit groups, the sign in bit 6 of the first), Pawn 3.3's opcodes with its packed forms (the operand in the high 32 bits
of the opcode cell), jump and call operands relative to the cell holding them, and native, library, public variable and
tag names given as 32-bit hashes (h = h * 131 ^ c over the name). Command names come from the player's own copy of the
game (orig/exefs/main); everything printed is derived from the game: keep it on this machine.
"""
import argparse
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "orig", "analysis", "script_commands.json")
HEADER = struct.Struct("<iHBBHHiiiiiiiiiiii")
FIELDS = ("size magic file_version amx_version flags defsize cod dat hea stp cip "
          "publics natives libraries pubvars tags nametable overlays").split()
MAGIC_64 = 0xF1E1
CELL = 8
MASK = (1 << 64) - 1

# Pawn 3.3's opcodes, in the game's order (its interpreter at 0x7100670710 has 215 handlers). Each entry: name, the number
# of operand cells, and what an operand is: "j" a jump target, "n" a native, "s" the native call's argument bytes, "c"
# a constant, "p" packed (the operand is in the opcode cell's high 32 bits, no operand cells).
_BASE = """
none:0 load.pri:1 load.alt:1 load.s.pri:1 load.s.alt:1 lref.pri:1 lref.alt:1 lref.s.pri:1 lref.s.alt:1 load.i:0 lodb.i:1
const.pri:1 const.alt:1 addr.pri:1 addr.alt:1 stor.pri:1 stor.alt:1 stor.s.pri:1 stor.s.alt:1 sref.pri:1 sref.alt:1
sref.s.pri:1 sref.s.alt:1 stor.i:0 strb.i:1 lidx:0 lidx.b:1 idxaddr:0 idxaddr.b:1 align.pri:1 align.alt:1 lctrl:1 sctrl:1
move.pri:0 move.alt:0 xchg:0 push.pri:0 push.alt:0 pick:1 push.c:1 push:1 push.s:1 pop.pri:0 pop.alt:0 stack:1 heap:1
proc:0 ret:0 retn:0 call:j jump.pri.unused:0 jump:j jrel:j jzer:j jnz:j jeq:j jneq:j jless:j jleq:j jgrtr:j jgeq:j
jsless:j jsleq:j jsgrtr:j jsgeq:j shl:0 shr:0 sshr:0 shl.c.pri:1 shl.c.alt:1 shr.c.pri:1 shr.c.alt:1 smul:0 sdiv:0
sdiv.alt:0 umul:0 udiv:0 udiv.alt:0 add:0 sub:0 sub.alt:0 and:0 or:0 xor:0 not:0 neg:0 invert:0 add.c:1 smul.c:1
zero.pri:0 zero.alt:0 zero:1 zero.s:1 sign.pri:0 sign.alt:0 eq:0 neq:0 less:0 leq:0 grtr:0 geq:0 sless:0 sleq:0 sgrtr:0
sgeq:0 eq.c.pri:1 eq.c.alt:1 inc.pri:0 inc.alt:0 inc:1 inc.s:1 inc.i:0 dec.pri:0 dec.alt:0 dec:1 dec.s:1 dec.i:0 movs:1
cmps:1 fill:1 halt:1 bounds:1 sysreq.pri:0 sysreq.c:n file:x line:2 symbol:x srange:2 jump.pri:0 switch:j casetbl:t
swap.pri:0 swap.alt:0 push.adr:1 nop:0 sysreq.n:ns symtag:1 break:0
push2.c:2 push2:2 push2.s:2 push2.adr:2 push3.c:3 push3:3 push3.s:3 push3.adr:3 push4.c:4 push4:4 push4.s:4 push4.adr:4
push5.c:5 push5:5 push5.s:5 push5.adr:5 load.both:2 load.s.both:2 const:2 const.s:2 icall:j iretn:0 iswitch:j icasetbl:t
load.p.pri:p load.p.alt:p load.p.s.pri:p load.p.s.alt:p lref.p.pri:p lref.p.alt:p lref.p.s.pri:p lref.p.s.alt:p
lodb.p.i:p const.p.pri:p const.p.alt:p addr.p.pri:p addr.p.alt:p stor.p.pri:p stor.p.alt:p stor.p.s.pri:p
stor.p.s.alt:p sref.p.pri:p sref.p.alt:p sref.p.s.pri:p sref.p.s.alt:p strb.p.i:p lidx.p.b:p idxaddr.p.b:p
align.p.pri:p align.p.alt:p push.p.c:p push.p:p push.p.s:p stack.p:p heap.p:p shl.p.c.pri:p shl.p.c.alt:p
shr.p.c.pri:p shr.p.c.alt:p add.p.c:p smul.p.c:p zero.p:p zero.p.s:p eq.p.c.pri:p eq.p.c.alt:p inc.p:p inc.p.s:p
dec.p:p dec.p.s:p movs.p:p cmps.p:p fill.p:p halt.p:p bounds.p:p push.p.adr:p sysreq.d:1 sysreq.nd:2
"""
OPCODES = []
for _item in _BASE.split():
    _name, _kind = _item.split(":")
    OPCODES.append((_name, _kind))
assert len(OPCODES) == 215, len(OPCODES)
BY_NAME = {name: i for i, (name, _) in enumerate(OPCODES)}
JUMPS = {i for i, (_, k) in enumerate(OPCODES) if k == "j"}


def operands(kind):
    """The number of operand cells an opcode of this kind has (casetbl and the debug ones are variable)."""
    if kind in ("p", "0"):
        return 0
    if kind in ("j", "n", "1"):
        return 1
    if kind == "ns":
        return 2
    if kind.isdigit():
        return int(kind)
    return None


def h131(name):
    """The 32-bit hash the game gives script names (natives, libraries, public variables, tags)."""
    h = 0
    for c in name.encode("latin-1"):
        h = ((h * 0x83) ^ c) & 0xFFFFFFFF
    return h


# --- compact encoding -----------------------------------------------------------------------------------------------

def expand(data):
    """The cells of a compact-encoded block."""
    cells = []
    i, n = 0, len(data)
    while i < n:
        start, c = i, 0
        while True:
            b = data[i]
            c = (c << 7) | (b & 0x7F)
            i += 1
            if not b & 0x80:
                break
        if data[start] & 0x40:
            c -= 1 << (7 * (i - start))
        cells.append(c & MASK)
    return cells


def compress(cells):
    """Pawn's compact encoding, as its compiler writes it (the fewest 7-bit groups that keep the sign)."""
    out = bytearray()
    for c in cells:
        t = [(c >> (7 * k)) & 0x7F for k in range(10)]
        index = 10
        while index > 1 and t[index - 1] == 0 and not t[index - 2] & 0x40:
            index -= 1
        if index == 10 and t[9] == 0x01 and t[8] & 0x40:
            index -= 1
        while index > 1 and t[index - 1] == 0x7F and t[index - 2] & 0x40:
            index -= 1
        for k in range(index - 1, -1, -1):
            out.append(t[k] | (0x80 if k else 0))
    return bytes(out)


def signed(c):
    return c - (1 << 64) if c & (1 << 63) else c


# --- the file -------------------------------------------------------------------------------------------------------

class Amx:
    """An AMX file: its header, its tables (address or value, 32-bit name hash), name table bytes, code and data."""

    def __init__(self, data):
        h = dict(zip(FIELDS, HEADER.unpack_from(data, 0)))
        if h["magic"] != MAGIC_64:
            raise ValueError("not a 64-bit Pawn AMX (magic %#x)" % h["magic"])
        if h["size"] != len(data):
            raise ValueError("size field %d, file %d bytes" % (h["size"], len(data)))
        self.header = h
        d = h["defsize"]

        def table(a, b):
            return [struct.unpack_from("<QI", data, at) for at in range(a, b, d)]
        self.publics = table(h["publics"], h["natives"])
        self.natives = table(h["natives"], h["libraries"])
        self.libraries = table(h["libraries"], h["pubvars"])
        self.pubvars = table(h["pubvars"], h["tags"])
        self.tags = table(h["tags"], h["nametable"])
        self.nametable = data[h["nametable"]:h["cod"]]
        self.overlays = None if h["overlays"] == h["nametable"] else h["overlays"]
        if h["flags"] & 0x04:
            cells = expand(data[h["cod"]:])
        else:
            cells = list(struct.unpack_from("<%dQ" % ((len(data) - h["cod"]) // 8), data, h["cod"]))
        if len(cells) * CELL != h["hea"] - h["cod"]:
            raise ValueError("code and data: %d bytes, the header says %d" % (len(cells) * CELL, h["hea"] - h["cod"]))
        ncode = (h["dat"] - h["cod"]) // CELL
        self.code = cells[:ncode]
        self.data = cells[ncode:]
        self.stack = h["stp"] - h["hea"]

    def build(self):
        """The file's bytes, laid out as the compiler lays them out."""
        h = dict(self.header)
        d = h["defsize"]
        at = HEADER.size
        h["publics"] = at
        at += d * len(self.publics)
        h["natives"] = at
        at += d * len(self.natives)
        h["libraries"] = at
        at += d * len(self.libraries)
        h["pubvars"] = at
        at += d * len(self.pubvars)
        h["tags"] = at
        at += d * len(self.tags)
        h["nametable"] = at
        h["overlays"] = at if self.overlays is None else self.overlays
        at += len(self.nametable)
        h["cod"] = at
        h["dat"] = h["cod"] + CELL * len(self.code)
        h["hea"] = h["dat"] + CELL * len(self.data)
        h["stp"] = h["hea"] + self.stack
        body = compress(self.code + self.data) if h["flags"] & 0x04 else struct.pack("<%dQ" % (len(self.code) + len(self.data)), *(self.code + self.data))
        h["size"] = h["cod"] + len(body)
        out = bytearray(HEADER.pack(*(h[f] for f in FIELDS)))
        for t in (self.publics, self.natives, self.libraries, self.pubvars, self.tags):
            for value, name in t:
                out += struct.pack("<QI", value, name) + bytes(d - 12)
        out += self.nametable
        out += body
        return bytes(out)


# --- instructions ---------------------------------------------------------------------------------------------------

def decode(code):
    """The instructions of a code section: (address, opcode, operand cells), with case tables as one instruction."""
    out = []
    i = 0
    n = len(code)
    while i < n:
        op = code[i] & 0xFFFFFFFF
        if op >= len(OPCODES):
            raise ValueError("bad opcode %#x at %#x" % (code[i], i * CELL))
        name, kind = OPCODES[op]
        if kind == "t":
            count = code[i + 1] & 0xFFFFFFFF
            size = 2 + 2 * count
        else:
            size = operands(kind)
            if size is None:
                raise ValueError("variable-length opcode %s at %#x" % (name, i * CELL))
        out.append((i * CELL, op, code[i + 1:i + 1 + size]))
        i += 1 + size
    if i != n:
        raise ValueError("the last instruction runs past the code")
    return out


def targets(insns):
    """Every code address a jump, call, switch or case refers to: {address: kind}."""
    found = {}
    for addr, op, ops in insns:
        name, kind = OPCODES[op]
        if op in JUMPS:
            found[addr + signed(ops[0])] = "proc" if name in ("call", "icall") else ("table" if "switch" in name else "label")
        elif kind == "t":
            # count, default, then (value, offset) pairs; an offset counts from its own cell, less 8
            cells = [addr + CELL * (1 + k) for k in range(len(ops))]
            found[cells[1] + signed(ops[1]) - CELL] = found.get(cells[1] + signed(ops[1]) - CELL, "label")
            for k in range(3, len(ops), 2):
                found[cells[k] + signed(ops[k]) - CELL] = found.get(cells[k] + signed(ops[k]) - CELL, "label")
    return found


# --- names ----------------------------------------------------------------------------------------------------------

# Public functions the game calls by name (0x710019b490 asks a zone script for its scene changes, four cells at a time,
# with GetSceneChangeData(index) until the first of four is -1).
PUBLIC_NAMES = {0x63F02D54: "GetSceneChangeData"}

# The game's script command tables ({name, function} pairs up to a null name) and the group each is in.
COMMAND_TABLES = {
    0x710153C878: "field", 0x710153AF78: "character", 0x710153B9F8: "party", 0x710153A898: "camera",
    0x710153B8A8: "item", 0x710153BFA0: "demo", 0x710153A710: "battle", 0x7101598E18: "float",
    0x7101598F48: "core", 0x7101598F28: "console", 0x71015990E0: "suspend", 0x71015D2BD8: "battle_ai",
}


def commands():
    """{hash: {"name", "group", "handler"}} for every script command the game registers, from orig/exefs/main."""
    if os.path.exists(CACHE):
        return {int(k, 16): v for k, v in json.load(open(CACHE)).items()}
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import nso2elf
    base = 0x7100000000
    nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
    img = nso["image"]
    rel = {r["offset"]: r["addend"] for r in nso2elf.analyze(nso)["relocs"] if r["type"] == 1027}
    out = {}
    for table, group in COMMAND_TABLES.items():
        at = table - base
        while at in rel:
            o = rel[at]
            name = img[o:img.index(b"\0", o)].decode("latin-1")
            f = rel.get(at + 8)
            out[h131(name)] = {"name": name, "group": group, "handler": "%#x" % (base + f) if f is not None else None}
            at += 16
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump({"%08x" % k: v for k, v in sorted(out.items())}, open(CACHE, "w"), indent=1)
    return out


# The battle AI scripts' one command, AI_CMD (0x7100a74140), and what its first argument picks: 0 to 122, read from the
# handler's switch (docs/BATTLE_AI.md). "who" is 0 the target, 1 the AI's own Pokemon, 2 the target's partner, 3 its own
# partner, 4 a Pokemon the AI is checking; "the move" is the move the AI is weighing. Names ending in a number are
# placeholders for what the code reads, not yet understood.
AI_COMMANDS = {
    0: ("IfRandomLessThan", "n", "a random number 0-255 is below n"),
    1: ("IfRandomGreaterThan", "n", "a random number 0-255 is above n"),
    2: ("IfRandomEqual", "n", "a random number 0-255 is n"),
    3: ("IfRandomNotEqual", "n", "a random number 0-255 isn't n"),
    4: ("IfHpPercentBelow", "who, percent", "its HP percent (rounded down) is below"),
    5: ("IfHpPercentAbove", "who, percent", "its HP percent (rounded down) is above"),
    6: ("IfHpPercentEqual", "who, percent", "its HP percent (rounded up) is"),
    7: ("IfHpPercentNotEqual", "who, percent", "its HP percent (rounded up) isn't"),
    8: ("IfHasStatus", "who", "it has a status (paralysis, sleep, freeze, burn, poison)"),
    9: ("IfNoStatus", "who", "it has no status"),
    10: ("IfHasCondition", "who, condition", "a condition is on (BattlePokemon::HasCondition)"),
    11: ("IfNotCondition", "who, condition", "a condition isn't on"),
    12: ("IfBadlyPoisoned", "who", "its poison is bad poison"),
    13: ("IfNotBadlyPoisoned", "who", "its poison isn't bad poison"),
    14: ("IfFlag", "who, flag", "one of its flags (bits at +0x303) is set"),
    15: ("IfNotFlag", "who, flag", "that flag isn't set"),
    16: ("IfSideCondition", "who, condition", "its side has a side condition (by number)"),
    17: ("IfNoSideCondition", "who, condition", "its side hasn't"),
    18: ("GetConditionValue20", "who", "condition 0x14's value when it's on, else 0"),
    19: ("GetConditionCounter20", "who", "condition 0x14's counter when it's on, else 0"),
    20: ("GetChoiceMove", "who", "the move a Choice item holds it to (condition 0x1c's value), else 0"),
    21: ("IfHasDamagingMove", "", "the AI's Pokemon knows a move with power"),
    22: ("IfHasNoDamagingMove", "", "it knows none"),
    23: ("GetTurnCount", "", "the battle's turn count"),
    24: ("GetType", "which", "a type: 0/2 the target's first/second, 1/3 its own, 4 the move's, 5/7 the target's partner's, 6/8 its partner's"),
    25: ("IfCanUseMove", "who, move", "it knows the move, has PP for it, and nothing stops it"),
    26: ("IfMoveHasPower", "move", "the move has power (isn't a status move)"),
    27: ("GetMovePower", "", "the move's power"),
    28: ("CompareMoveDamage", "", "2 when the move does the most damage of the AI's moves, 1 when another does more, 0 when it does none"),
    29: ("GetLockedMove", "who", "the move it's locked into (condition 0x1a), else 0"),
    30: ("IfFaster", "which", "speeds: 0 the AI's Pokemon is faster, 1 the target is, 2 the same"),
    31: ("CountPartyAbleToFight", "who", "how many of its side's Pokemon not in battle can fight"),
    32: ("GetMoveField28", "", "the move's data field 0x1c"),
    33: ("GetAbility", "who", "its ability: its own side's real one; an opponent's as shown, else one its species can have"),
    34: ("IfEffectiveness", "attacker, defender, move, value", "type effectiveness (7 neutral, each step up doubling): 9 means above 8, 5 below 6, else equal"),
    35: ("GetEffectiveness", "attacker, defender, move", "the move's type effectiveness (7 neutral; 7 when it can't tell)"),
    36: ("IfHasMoreEffectiveMove", "who, target, value", "it has a usable move whose effectiveness on the target is above value"),
    37: ("IfHasMoveOfEffectiveness", "who, target, value", "it has a usable move whose effectiveness on the target is value"),
    38: ("IfPartyHasStatus", "who", "a Pokemon of its side not in battle has a status"),
    39: ("IfPartyHasNoStatus", "who", "none has"),
    40: ("GetWeather", "", "the weather (0 when Air Lock suppresses it)"),
    41: ("IfValueBelow", "who, value, n", "a value (BattlePokemon::GetValue: 1-7 stages with 6 none, 8 Attack, 9 Defense, 10 Sp. Atk, 11 Sp. Def, 12 Speed, 13 HP, 14 max HP, 15 level, 17 ability) is below n"),
    42: ("IfValueAbove", "who, value, n", "the value is above n"),
    43: ("IfValueEqual", "who, value, n", "the value is n"),
    44: ("IfValueNotEqual", "who, value, n", "the value isn't n"),
    45: ("IfMoveKnocksOut", "", "the move's damage, as the AI works it out, is at least the target's HP"),
    46: ("IfMoveDoesntKnockOut", "", "it isn't"),
    47: ("IfKnowsMove", "who, move", "it knows the move (an opponent: one the AI has seen it use)"),
    48: ("IfDoesntKnowMove", "who, move", "it doesn't"),
    49: ("If76ed0", "who, x", "0x7100a76ed0"),
    50: ("IfNot76ed0", "who, x", "not 0x7100a76ed0"),
    51: ("Stop", "", "sets the AI's done flag (+0x220); returns 0"),
    52: ("GetHeldItem", "who", "its held item: always 0 in Let's Go"),
    53: ("GetHeldItemEffect", "who", "its held item's effect: 0"),
    54: ("GetValue18", "who", "value 0x12"),
    55: ("IfFlag0", "who", "flag bit 0 at +0x303"),
    56: ("GetCounter0", "who", "counter 0 at +0x307"),
    57: ("GetBattleRule", "", "the battle's rule (1 to 3 have partners)"),
    58: ("GetBattleKind", "", "a value of the battle's setup (+0x80); 113 asks whether it's 1"),
    59: ("GetZero", "who", "always 0 (0x71009e6da0)"),
    60: ("GetMoveType", "", "the move's type"),
    61: ("GetMovePower2", "", "the move's power, as 27"),
    62: ("GetMoveField28_2", "", "the move's data field 0x1c, as 32"),
    63: ("GetCounter3", "who", "counter 3 at +0x307"),
    64: ("IfHigherLevel", "which", "levels: 0 the AI's Pokemon's is higher, 1 the target's is, 2 the same"),
    65: ("IfTargetHasCondition11", "", "condition 0xb is on the target"),
    66: ("IfTargetHasCondition11_2", "", "the same as 65"),
    67: ("IfPositionsLinked", "", "0x71009e2000 on its own and another position"),
    68: ("IfHasType", "who, type", "one of its three types is type (18, none, never)"),
    69: ("IfAbility", "who, ability", "its ability, as 33, is"),
    70: ("IfFlag13", "who", "flag bit 0xd at +0x303"),
    71: ("IfHeldItem", "who, item", "its held item is item (always 0 in Let's Go)"),
    72: ("IfFieldEffect", "effect", "a field effect is on (6 Wonder Room, ...)"),
    73: ("GetSideCondition", "who, condition", "the side condition's count (layers or turns)"),
    74: ("IfPartyHasHurt", "who", "a Pokemon of its side not in battle can fight and has lost HP"),
    75: ("IfPartyHasUsedPp", "who", "a Pokemon of its side not in battle has used PP"),
    76: ("GetHeldItemValue6", "who", "its item's value 6 unless condition 0x13 (Embargo) is on: 0 in Let's Go"),
    77: ("GetMovePp", "", "the PP left of the move"),
    78: ("IfMovesUsed4800", "who", "it knows more than one move and 0x71009e4800 is at least their count less one"),
    79: ("GetMoveCategory", "", "the move's category (0 status, 1 physical, 2 special)"),
    80: ("GetTargetLockedMoveCategory", "", "the category of the move the target is locked into"),
    81: ("GetValueA0a1f0", "who", "0x7100a0a1f0 for it"),
    82: ("GetValue2E8", "who", "its 16 bits at +0x2e8"),
    83: ("IfPartyHasHarderHitter", "flag", "one of its side's Pokemon not in battle would hurt the target more"),
    84: ("IfHasSuperEffectiveMove", "who, target", "one of its moves is super effective on the target"),
    85: ("IfLockedMoveHitsHarder", "who, flag", "the move it's locked into would hurt the target more than the AI's Pokemon's best"),
    86: ("SumRaisedStages", "who", "how many stages its 7 stats are raised, added up"),
    87: ("GetValueDifference", "who, value", "its value less the AI's Pokemon's"),
    88: ("GetValue", "who, value", "a value, as 41"),
    89: ("ComparePartyMoveDamage", "flag", "2/1/0 as 28, over the side's Pokemon"),
    90: ("IfFainted", "who", "it has fainted"),
    91: ("IfNotFainted", "who", "it hasn't"),
    92: ("GetAbilityNow", "who", "value 17: the ability it has now"),
    93: ("IfHasSubstitute", "who", "it's behind a substitute"),
    94: ("GetSpecies", "who", "its species"),
    95: ("GetForm", "who", "its form"),
    96: ("IfGameValueBelow", "n", "the value at 0x71016490f0 is below n"),
    97: ("IfGameValueAbove", "n", "it's above n"),
    98: ("IfGameValueEqual", "n", "it's n"),
    99: ("IfGameValueNotEqual", "n", "it isn't n"),
    100: ("IfPositionEffect3", "who", "0x7100a0a570 with 3 for its position"),
    101: ("IfAttackBelowSpAtk", "who", "its Attack is below its Sp. Atk"),
    102: ("IfAttackAboveSpAtk", "who", "its Attack is above its Sp. Atk"),
    103: ("IfAttackNotSpAtk", "who", "they differ"),
    104: ("IfFlag2_0", "who", "flag bit 0 at +0x306"),
    105: ("IfValue26E", "who, n", "its byte at +0x26e is n"),
    106: ("IfHandler10", "n", "0x7100a689d0 with 10 and n"),
    107: ("GetValue2DE", "who", "its 16 bits at +0x2de"),
    108: ("IfSetupFlag65", "", "a flag of the battle's setup (+0x80, +0x65)"),
    109: ("IfPokemonFlag728900", "who", "0x7100728900 on its Pokemon data"),
    110: ("If696e0", "who", "0x7100a696e0 for it"),
    111: ("IfState5420", "who", "0x71009e5420 isn't 0x18"),
    112: ("GetHiddenPowerType", "who", "its Hidden Power type"),
    113: ("IfBattleKind1", "", "58 is 1"),
    114: ("GetMax76fc0", "who", "the larger of 0x7100a76fc0 against the target and the target's partner"),
    115: ("IfMoveAbsorbed", "who, move", "its ability takes the move's type: Ground Levitate, Fire Flash Fire, Water Water Absorb/Storm Drain/Dry Skin, Grass Sap Sipper, Electric Volt Absorb/Lightning Rod/Motor Drive"),
    116: ("Get6f90", "who", "0x71009e6f90's value when it says yes, else 0"),
    117: ("GetMove", "", "the move"),
    118: ("GetValue90", "", "the AI's 16 bits at +0x90"),
    119: ("IfAbc0", "who", "0x7100a0abc0 with 1, 2 or 3 for it"),
    120: ("CountParty5b0", "who", "0x71009df5b0 on its side's Pokemon"),
    121: ("GetMove0500", "", "0x71009d0500 for the move and the AI's Pokemon"),
    122: ("IfHasSuperEffectiveMove2", "who, target", "as 84, through 0x7100a09a50"),
}


def ai_notes(text):
    """The disassembly of a battle AI script with each AI_CMD call it makes through a wrapper (a function that hands its
    arguments to AI_CMD) noted with the subcommand's name, from the constant pushed first."""
    lines = text.split("\n")
    wrappers, current = set(), None
    for line in lines:
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*:$", line):
            current = line[:-1]
        elif current and re.match(r"^\s+sysreq\.[a-z]+ AI_CMD\b", line):
            wrappers.add(current)
    if not wrappers:
        return text
    out = []
    for line in lines:
        m = re.match(r"^(\s+call ([A-Za-z_][A-Za-z0-9_]*))\s*$", line)
        if m and m.group(2) in wrappers:
            before = [l.strip() for l in out[-2:]]
            sub = re.match(r"^push\.c (\d+)$", before[0]) if len(before) == 2 else None
            if sub and re.match(r"^push\.c \d+$", before[1]) and int(sub.group(1)) in AI_COMMANDS:
                n = int(sub.group(1))
                line = "%-24s ; AI_CMD %d: %s" % (m.group(1), n, AI_COMMANDS[n][0])
        out.append(line)
    return "\n".join(out)


HACK_COMMAND_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*[A-Za-z_][A-Za-z0-9_:]*\s*\}')


def hack_commands():
    """{hash: {"name", "group"}} for the commands hacks add in C++: the {"Name", Function} entries of the tables a hack
    registers through 0x7100674ae0 (hacks/new-script-command has the pattern). A script can call one only in a build
    with that hack."""
    out = {}
    hacks = os.path.join(ROOT, "hacks")
    for hack in sorted(os.listdir(hacks)) if os.path.isdir(hacks) else []:
        code = os.path.join(hacks, hack, "code")
        for f in sorted(os.listdir(code)) if os.path.isdir(code) else []:
            if not f.endswith(".cpp"):
                continue
            text = open(os.path.join(code, f), encoding="utf-8").read()
            if "ORIG_7100674ae0" not in text:
                continue
            for name in HACK_COMMAND_RE.findall(text):
                out[h131(name)] = {"name": name, "group": "hack " + hack, "handler": None}
    return out


def all_commands():
    """The game's commands and the hacks' together, for naming and for calls without a .native line."""
    out = commands()
    for k, v in hack_commands().items():
        out.setdefault(k, v)
    return out


# --- source ---------------------------------------------------------------------------------------------------------

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
COMMAND_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(@[0-9]+)?$")   # a table can list a command twice: float, float@2


def command_hash(name):
    base = COMMAND_RE.match(name).group(1)
    return int(base[7:], 16) if re.match(r"^native_[0-9a-f]{8}$", base) else h131(base)


# --- readable statements ------------------------------------------------------------------------------------------
#
# Calls are written as statements: `name(arg, ...)`, `target = name(arg, ...)`, `if name(...) goto label` and
# `ifnot name(...) goto label`, where name is a command (a native) or a function's label, and each argument is a number,
# argN (the function's Nth argument), varN (its Nth local, at frm-8N), g_XXXX (the global at data offset XXXX), &argN
# and &varN (their addresses), a "string" or a data label (their addresses). A statement stands for exactly the
# instructions the compiler wrote for it: a script
# function's arguments are pushed last to first, then their size in bytes, then `call`; a command's arguments the same
# way, then `sysreq.n`. The disassembler writes a statement only where it assembles back to the same instructions.

PUSH_FORMS = {"push.p.c": "c", "push.c": "c", "push.p.s": "s", "push.s": "s", "push.p": "g", "push": "g",
              "push.p.adr": "a", "push.adr": "a"}


# --- strings --------------------------------------------------------------------------------------------------------
#
# Pawn keeps a string in the data section one character a cell, ending with a 0 cell. The disassembler writes each one
# the code refers to as `.string "..."` under an s_XXXX label (XXXX its data offset), and an argument or constant that
# is a string's address as the string itself when no other string has the same text (else as its label). The assembler
# puts each `.string` where it stands, finds a string argument's text among them (adding it at the end of the data when
# it is new), and takes a data label anywhere a number goes. Escapes: \n \r \t \\ \" \xHH \uHHHH. An array of strings
# starts with a table of offsets, one per string, so its address stays a number.

STRING_FORMS = {"push.p.c", "push.c", "const.pri", "const.alt", "const.p.pri", "const.p.alt"}
ESCAPES = {"\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t", "\r": "\\r"}
UNESCAPES = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"'}
LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):(.*)$")


def is_text(c):
    return c in (9, 10, 13) or 0x20 <= c < 0x7F      # the scripts' strings are ASCII (names, printf formats)


def offset_table(cells):
    """Whether cells start like the offsets of an array of strings (each a multiple of 8, rising, the first counting
    the table itself) rather than like text."""
    n = cells[0] // CELL if cells and cells[0] % CELL == 0 else 0
    head = cells[:n]
    return n >= 2 and len(head) == n and all(c % CELL == 0 for c in head) and all(b > a for a, b in zip(head, head[1:]))


def quote(text):
    out = []
    for ch in text:
        if ch in ESCAPES:
            out.append(ESCAPES[ch])
        elif 0x20 <= ord(ch) < 0x7F or ord(ch) >= 0xA0:
            out.append(ch)
        else:
            out.append("\\x%02x" % ord(ch))
    return '"' + "".join(out) + '"'


def unquote(token):
    if len(token) < 2 or token[0] != '"' or token[-1] != '"':
        raise SourceError("bad string " + token)
    body = token[1:-1]
    out = []
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == '"':
            raise SourceError("a quote inside a string needs a backslash: " + token)
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        e = body[i + 1:i + 2]
        if e in UNESCAPES:
            out.append(UNESCAPES[e])
            i += 2
        elif e in ("x", "u") and re.match(r"^[0-9A-Fa-f]{%d}$" % (2 if e == "x" else 4), body[i + 2:i + (4 if e == "x" else 6)]):
            n = 2 if e == "x" else 4
            out.append(chr(int(body[i + 2:i + 2 + n], 16)))
            i += 2 + n
        else:
            raise SourceError("bad escape in " + token)
    return "".join(out)


def strip_comment(line):
    """A source line without its comment (a ';' outside a string and what follows)."""
    quoted = escape = False
    for i, ch in enumerate(line):
        if quoted:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif ch == ";":
            return line[:i]
    return line


def split_outside(text, is_separator):
    """text split at the characters is_separator accepts, except inside strings."""
    parts = []
    start = 0
    quoted = escape = False
    for i, ch in enumerate(text):
        if quoted:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif is_separator(ch):
            parts.append(text[start:i])
            start = i + 1
    if quoted:
        raise SourceError("a string without its closing quote: " + text)
    parts.append(text[start:])
    return parts


def words(text):
    return [w for w in split_outside(text, str.isspace) if w]


def find_strings(amx, insns):
    """{data offset: text} for the strings the code refers to: a push or const of a data offset where a run of text
    cells starts (after a 0, a cell that isn't text or a public variable) and ends with a 0 cell, not counting the
    byte count pushed before a call. An offset inside a run is a number that happens to match, or a pointer into a
    string: it stays a number."""
    refs = set()
    for i, (addr, op, ops) in enumerate(insns):
        name, kind = OPCODES[op]
        if name not in STRING_FORMS:
            continue
        if name in ("push.p.c", "push.c") and i + 1 < len(insns) and OPCODES[insns[i + 1][1]][0] in ("call", "icall"):
            continue
        v = signed(amx.code[addr // CELL]) >> 32 if kind == "p" else signed(ops[0])
        if v >= 0 and v % CELL == 0 and v // CELL < len(amx.data):
            refs.add(v)
    pubvar_at = {v for v, _ in amx.pubvars}
    found = {}
    for start in sorted(refs - pubvar_at):
        k = start // CELL
        if k > 0 and is_text(amx.data[k - 1]) and start - CELL not in pubvar_at:
            continue
        chars = []
        while k < len(amx.data) and amx.data[k] != 0 and is_text(amx.data[k]):
            chars.append(chr(amx.data[k]))
            k += 1
        if (chars and k < len(amx.data) and amx.data[k] == 0 and not any(start < p <= k * CELL for p in pubvar_at)
                and not offset_table([ord(ch) for ch in chars])):
            found[start] = "".join(chars)
    return found


def fits32(v):
    return -0x80000000 <= v <= 0x7FFFFFFF


def show_number(v):
    return "%d" % v if -0x10000 < v < 0x10000 else "%#x" % (v & MASK)


def frame_name(off):
    if off >= 24 and (off - 24) % CELL == 0:
        return "arg%d" % ((off - 24) // CELL)
    if off < 0 and off % CELL == 0:
        return "var%d" % (-off // CELL)
    return None


def frame_offset(name):
    m = re.match(r"^(arg|var)([0-9]+)$", name)
    if not m:
        return None
    k = int(m.group(2))
    return 24 + CELL * k if m.group(1) == "arg" else (-CELL * k if k > 0 else None)


def operand_text(form, v):
    """An argument as written in a statement, or None when the push has no such form."""
    if form == "c":
        return show_number(v)
    if form == "s":
        return frame_name(v)
    if form == "a":
        n = frame_name(v)
        return "&" + n if n else None
    if form == "g":
        return "g_%04x" % v if v >= 0 and v % CELL == 0 else None
    return None


def lower_operand(text):
    """The push instruction (in the disassembler's raw form) an argument stands for."""
    if text.startswith("&"):
        off = frame_offset(text[1:])
        if off is None:
            raise SourceError("bad address argument " + text)
        return "push.p.adr %d" % off
    if text.startswith("g_"):
        addr = int(text[2:], 16)
        return ("push.p %d" if fits32(addr) else "push %d") % addr
    off = frame_offset(text)
    if off is not None:
        return "push.p.s %d" % off
    if text.startswith('"'):
        unquote(text)
        return "push.p.c " + text      # a string's address
    if NAME_RE.match(text):
        return "push.p.c " + text      # a data label's address
    try:
        v = signed(int(text, 0) & MASK)
    except ValueError:
        raise SourceError("bad argument " + text) from None
    return ("push.p.c %d" if fits32(v) else "push.c %d") % v


def lower_store(text):
    if text.startswith("g_"):
        addr = int(text[2:], 16)
        return ("stor.p.pri %d" if fits32(addr) else "stor.pri %d") % addr
    off = frame_offset(text)
    if off is None:
        raise SourceError("bad assignment target " + text)
    return "stor.p.s.pri %d" % off


STATEMENT_RE = re.compile(r"^(?:(if|ifnot)\s+)?(?:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*)?([A-Za-z_][A-Za-z0-9_@]*)\((.*)\)"
                          r"(?:\s+goto\s+([A-Za-z_][A-Za-z0-9_]*))?$")


def lower_statement(text, natives):
    """The raw instructions a statement stands for, or None when the line isn't a statement."""
    m = STATEMENT_RE.match(text)
    if not m:
        return None
    cond, target, name, args, label = m.groups()
    if bool(cond) != bool(label):
        raise SourceError("`if`/`ifnot` needs `goto label`, and `goto` needs `if` or `ifnot`")
    if cond and target:
        raise SourceError("a conditional call can't also assign")
    args = [a.strip() for a in split_outside(args, lambda ch: ch == ",")] if args.strip() else []
    out = [lower_operand(a) for a in reversed(args)]
    if name in natives:
        out.append("sysreq.n %s %d" % (name, CELL * len(args)))
    else:
        out.append("push.p.c %d" % (CELL * len(args)))
        out.append("call %s" % name)
    if target:
        out.append(lower_store(target))
    if cond:
        out.append("%s %s" % ("jnz" if cond == "if" else "jzer", label))
    return out


def disassemble(amx, names=None, readable=True):
    """Editable source for a script: every field the file needs, labels for jump targets, commands by name, and calls
    written as statements (readable=False keeps every instruction as it is)."""
    names = names or {}
    insns = decode(amx.code)
    starts = {a for a, _, _ in insns}
    tg = targets(insns)
    bad = [a for a in tg if a not in starts]
    if bad:
        raise ValueError("jump targets inside instructions: %s" % ", ".join("%#x" % a for a in bad[:5]))
    main = amx.header["cip"]
    label = {}
    for a, kind in sorted(tg.items()):
        label[a] = ("main" if a == main else "f_%04x" % a) if kind == "proc" else ("t_%04x" % a if kind == "table" else "l_%04x" % a)
    if main not in label and main >= 0:
        label[main] = "main"
    # Public functions, which the game calls by name, are labelled with it so they move with the code.
    for value, hv in amx.publics:
        if value not in starts:
            raise ValueError("public %#010x at %#x is not an instruction" % (hv, value))
        if value != main:
            label[value] = PUBLIC_NAMES.get(hv, "public_%08x" % hv)

    # Strings: an address is written as the text when no other string has the same one, else as the label.
    strings = find_strings(amx, insns)
    same_text = {}
    for text in strings.values():
        same_text[text] = same_text.get(text, 0) + 1
    string_ok = set()      # instructions whose constant may be written as a string (not a call's byte count)
    for i, (addr, op, ops) in enumerate(insns):
        nm = OPCODES[op][0]
        if nm in STRING_FORMS and not (nm in ("push.p.c", "push.c") and i + 1 < len(insns)
                                       and OPCODES[insns[i + 1][1]][0] in ("call", "icall")):
            string_ok.add(i)

    def constant(i, v):
        if i in string_ok and v in strings:
            return quote(strings[v]) if same_text[strings[v]] == 1 else "s_%04x" % v
        return "%d" % v

    def native(i):
        h = amx.natives[i][1]
        n = names.get(h, {}).get("name")
        return n if n and NAME_RE.match(n) and h131(n) == h else "native_%08x" % h

    native_names = []
    for i in range(len(amx.natives)):
        n = native(i)
        k = native_names.count(n) + sum(1 for m in native_names if m.startswith(n + "@"))
        native_names.append(n if k == 0 else "%s@%d" % (n, k + 1))
    natives = set(native_names)

    # Functions that only wrap one command ending in an underscore take the command's name without it
    # (StartTurnAround_ -> StartTurnAround), as the game's own include files name them.
    procs = [a for a, op, _ in insns if OPCODES[op][0] == "proc"]
    index_of = {a: i for i, (a, _, _) in enumerate(insns)}
    taken = set(label.values()) | natives
    for k, p in enumerate(procs):
        if p not in label or not label[p].startswith("f_"):
            continue
        end = procs[k + 1] if k + 1 < len(procs) else len(amx.code) * CELL
        cmds = []
        for i in range(index_of[p], len(insns)):
            a, op, ops = insns[i]
            if a >= end:
                break
            nm = OPCODES[op][0]
            if nm in ("call", "sysreq.c", "sysreq.pri", "icall"):
                cmds.append(None)
            elif nm == "sysreq.n":
                cmds.append(native_names[ops[0]] if ops[0] < len(native_names) else None)
        if len(cmds) == 1 and cmds[0] and cmds[0].endswith("_") and not cmds[0].startswith("native_") and "@" not in cmds[0]:
            new = cmds[0][:-1]
            if new and new not in taken and NAME_RE.match(new):
                taken.add(new)
                label[p] = new

    # Raw text of each instruction (case tables apart).
    raw = []
    for i, (addr, op, ops) in enumerate(insns):
        name, kind = OPCODES[op]
        hi = amx.code[addr // CELL] >> 32
        if kind != "p" and hi:
            raise ValueError("unpacked opcode with high bits at %#x" % addr)
        if kind == "p":
            raw.append("%s %s" % (name, constant(i, signed(amx.code[addr // CELL]) >> 32)))
        elif op in JUMPS:
            raw.append("%s %s" % (name, label[addr + signed(ops[0])]))
        elif kind == "n":
            raw.append("%s %s" % (name, native_names[ops[0]] if ops[0] < len(native_names) else ops[0]))
        elif kind == "ns":
            raw.append("%s %s %d" % (name, native_names[ops[0]] if ops[0] < len(native_names) else ops[0], ops[1]))
        elif kind == "t":
            raw.append(None)
        elif len(ops) == 1:
            raw.append("%s %s" % (name, constant(i, signed(ops[0]))))
        else:
            raw.append(name + "".join(" %d" % signed(o) for o in ops))

    # Arguments each function is called with, from its call sites.
    arg_bytes = {}
    for i, (addr, op, ops) in enumerate(insns):
        if OPCODES[op][0] == "call" and i > 0 and raw[i - 1] and raw[i - 1].startswith("push.p.c "):
            arg_bytes.setdefault(addr + signed(ops[0]), set()).add(int(raw[i - 1].split()[1]))

    def push_text(i):
        """The argument a push stands for in a statement, or None."""
        addr, op, ops = insns[i]
        name, kind = OPCODES[op]
        v = signed(amx.code[addr // CELL]) >> 32 if kind == "p" else signed(ops[0])
        if PUSH_FORMS[name] == "c" and i in string_ok and v in strings:
            return constant(i, v)
        return operand_text(PUSH_FORMS[name], v)

    # Fold calls into statements.
    out = {}       # first instruction index -> (statement text, count of instructions it covers)
    if readable:
        n = len(insns)
        for j in range(n):
            if not raw[j]:
                continue
            parts = raw[j].split()
            if parts[0] not in ("sysreq.n", "call"):
                continue
            # the simple pushes just before the call
            pushes = []
            x = j - 1
            while x >= 0 and raw[x] and OPCODES[insns[x][1]][0] in PUSH_FORMS and x not in out:
                pushes.insert(0, push_text(x))
                x -= 1
                if x + 1 < j and insns[x + 1][0] in label:
                    break
            if parts[0] == "sysreq.n":
                if int(parts[2]) % CELL:
                    continue
                k = int(parts[2]) // CELL
                if k > len(pushes):
                    continue
                args = pushes[len(pushes) - k:][::-1]
                first = j - k
            else:
                if not raw[j - 1] or not raw[j - 1].startswith("push.p.c "):
                    continue
                nbytes = int(raw[j - 1].split()[1])
                if nbytes % CELL or nbytes // CELL > len(pushes) - 1:
                    continue
                k = nbytes // CELL
                args = pushes[len(pushes) - 1 - k:len(pushes) - 1][::-1]
                first = j - 1 - k
            if any(a is None for a in args) or any(y in out for y in range(first, j + 1)):
                continue
            text = "%s(%s)" % (parts[1], ", ".join(args))
            cover = j - first + 1
            nxt = j + 1
            if nxt < n and raw[nxt] and insns[nxt][0] not in label:
                mn = raw[nxt].split()
                if mn[0] == "stor.p.s.pri" and frame_name(int(mn[1])):
                    text = "%s = %s" % (frame_name(int(mn[1])), text)
                    cover += 1
                elif mn[0] == "stor.p.pri" and int(mn[1]) >= 0 and int(mn[1]) % CELL == 0:
                    text = "g_%04x = %s" % (int(mn[1]), text)
                    cover += 1
                elif mn[0] in ("jzer", "jnz"):
                    text = "%s %s goto %s" % ("ifnot" if mn[0] == "jzer" else "if", text, mn[1])
                    cover += 1
            inner = [insns[y][0] for y in range(first + 1, first + cover)]
            if any(a in label for a in inner):
                continue
            try:
                same = lower_statement(text, natives) == raw[first:first + cover]
            except SourceError:
                same = False
            if same:
                out[first] = (text, cover)

    h = amx.header
    lines = ["; Pawn script for Pokémon: Let's Go, written by tools/amx.py. Edit it and assemble it back with",
             "; `python -P tools/amx.py asm`. Labels name code addresses; calls are written as statements",
             "; (docs/SCRIPTS.md has the syntax).",
             ".amx file_version=%d amx_version=%d flags=%#x defsize=%d" % (h["file_version"], h["amx_version"], h["flags"], h["defsize"]),
             ".stack %#x" % amx.stack,
             ".nametable %s" % amx.nametable.hex()]
    if amx.overlays is not None:
        lines.append(".overlays %#x" % amx.overlays)
    if main < 0:
        lines.append(".nomain")
    for value, name in amx.publics:
        if h131(label[value]) == name:
            lines.append(".public %s" % label[value])
        else:
            lines.append(".public %s %#010x" % (label[value], name))
    for i, n in enumerate(native_names):
        lines.append(".native %s" % n + ("" if n.startswith("native_") else "    ; %#010x" % amx.natives[i][1]))
    for value, name in amx.libraries:
        lines.append(".library %#010x %#x" % (name, value))
    for value, name in amx.pubvars:
        lines.append(".pubvar %#010x %#x" % (name, value))
    for value, name in amx.tags:
        lines.append(".tag %#010x %#x" % (name, value))
    lines.append("")
    lines.append(".code")
    i = 0
    while i < len(insns):
        addr, op, ops = insns[i]
        name, kind = OPCODES[op]
        if addr in label:
            if label[addr] == "main" or name == "proc":
                lines.append("")
                sizes = arg_bytes.get(addr)
                note = ""
                if sizes and len(sizes) == 1:
                    k = next(iter(sizes)) // CELL
                    note = "    ; %d argument%s" % (k, "" if k == 1 else "s")
                lines.append("%s:%s" % (label[addr], note))
            else:
                lines.append("%s:" % label[addr])
        if i in out:
            text, cover = out[i]
            lines.append("    " + text)
            i += cover
            continue
        if kind == "t":
            cells = [addr + CELL * (1 + k) for k in range(len(ops))]
            lines.append("    %s" % name)
            lines.append("        default %s" % label[cells[1] + signed(ops[1]) - CELL])
            for k in range(2, len(ops), 2):
                lines.append("        case %s %s" % (show_number(signed(ops[k])), label[cells[k + 1] + signed(ops[k + 1]) - CELL]))
        else:
            lines.append("    " + raw[i])
        i += 1
    lines.append("")
    lines.append(".data")
    row = []
    k = 0
    while k <= len(amx.data):
        if row and (k == len(amx.data) or k * CELL in strings or len(row) == 8):
            lines.append("    .cell " + " ".join(row))
            row = []
        if k == len(amx.data):
            break
        if k * CELL in strings:
            text = strings[k * CELL]
            lines.append("s_%04x:" % (k * CELL))
            lines.append("    .string " + quote(text))
            k += len(text) + 1
            continue
        row.append("%d" % signed(amx.data[k]))
        k += 1
    return "\n".join(lines) + "\n"


class SourceError(Exception):
    pass


def number(text):
    return int(text, 0)


def assemble(source, names=None):
    """An AMX's bytes from source written by disassemble (or by hand in its form)."""
    known = {v["name"] for v in (names or {}).values()}   # the game's commands, callable without a .native line
    h = {"magic": MAGIC_64, "file_version": 10, "amx_version": 10, "flags": 0x1C, "defsize": 12, "overlays": None}
    stack = 0x2000
    nametable = b"\x3f\x00\x00\x00"
    publics, natives, libraries, pubvars, tags = [], [], [], [], []
    native_index = {}
    section = None
    items = []          # (line number, label or None, mnemonic, operands)
    data = []
    data_labels = {}    # name -> data offset
    string_at = {}      # text -> the data offset of its first .string
    for no, raw in enumerate(source.splitlines(), 1):
        try:
            line = strip_comment(raw).strip()
            if not line:
                continue
            if section == "data":
                m = LABEL_RE.match(line)
                if m:
                    if m.group(1) in data_labels:
                        raise SourceError("label %s twice" % m.group(1))
                    data_labels[m.group(1)] = len(data) * CELL
                    line = m.group(2).strip()
                    if not line:
                        continue
            if line.startswith("."):
                word, _, rest = line.partition(" ")
                args = words(rest)
                if word == ".string":
                    if section != "data":
                        raise SourceError(".string outside .data")
                    text = unquote(rest.strip())
                    string_at.setdefault(text, len(data) * CELL)
                    data.extend(ord(ch) for ch in text)
                    data.append(0)
                elif word == ".amx":
                    for a in args:
                        k, v = a.split("=")
                        h[k] = number(v)
                elif word == ".stack":
                    stack = number(args[0])
                elif word == ".nametable":
                    nametable = bytes.fromhex(args[0])
                elif word == ".nomain":
                    pass
                elif word == ".overlays":
                    h["overlays"] = number(args[0])
                elif word == ".public":
                    publics.append((args[0], number(args[1]) if len(args) > 1 else h131(args[0])))
                elif word == ".native":
                    n = args[0]
                    if not COMMAND_RE.match(n):
                        raise SourceError("bad command name " + n)
                    native_index[n] = len(natives)
                    natives.append(command_hash(n))
                elif word == ".library":
                    libraries.append((number(args[1]), number(args[0])))
                elif word == ".pubvar":
                    pubvars.append((number(args[1]), number(args[0])))
                elif word == ".tag":
                    tags.append((number(args[1]), number(args[0])))
                elif word == ".code":
                    section = "code"
                elif word == ".data":
                    section = "data"
                elif word == ".cell":
                    if section != "data":
                        raise SourceError(".cell outside .data")
                    data.extend(number(a) & MASK for a in args)
                else:
                    raise SourceError("unknown directive " + word)
                continue
            if section != "code":
                raise SourceError("an instruction outside .code")
            lab = None
            m = LABEL_RE.match(line)
            if m:
                lab, line = m.group(1), m.group(2).strip()
                if not line:
                    items.append((no, lab, None, []))
                    continue
            lowered = lower_statement(line, set(native_index) | known)
            if lowered is not None:
                for k, ins in enumerate(lowered):
                    parts = words(ins)
                    items.append((no, lab if k == 0 else None, parts[0], parts[1:]))
                continue
            parts = words(line)
            items.append((no, lab, parts[0], parts[1:]))
        except (ValueError, IndexError) as e:
            raise SourceError("line %d: %s" % (no, e)) from None
    # Lay the code out: each instruction's size, then the labels' addresses.
    layout = []
    addr = 0
    labels = {}
    i = 0
    while i < len(items):
        no, lab, mnem, args = items[i]
        if lab:
            if lab in labels:
                raise SourceError("line %d: label %s twice" % (no, lab))
            labels[lab] = addr
        if mnem is None:
            i += 1
            continue
        if mnem in ("default", "case"):
            raise SourceError("line %d: %s outside a casetbl" % (no, mnem))
        if mnem not in BY_NAME:
            raise SourceError("line %d: unknown instruction %s" % (no, mnem))
        op = BY_NAME[mnem]
        kind = OPCODES[op][1]
        if kind == "t":
            cases = []
            default = None
            i += 1
            while i < len(items) and items[i][2] in ("default", "case") and not items[i][1]:
                _, _, m, a = items[i]
                if m == "default":
                    default = a[0]
                else:
                    cases.append((number(a[0]), a[1]))
                i += 1
            if default is None:
                raise SourceError("line %d: casetbl without a default" % no)
            layout.append((no, addr, op, ("table", default, cases)))
            addr += CELL * (3 + 2 * len(cases))
            continue
        size = 0 if kind == "p" else operands(kind)
        if size is None:
            raise SourceError("line %d: %s can't be assembled" % (no, mnem))
        layout.append((no, addr, op, args))
        addr += CELL * (1 + size)
        i += 1
    code = []

    def target(no, name):
        if name not in labels:
            raise SourceError("line %d: no label %s" % (no, name))
        return labels[name]

    def native(no, name):
        if name in native_index:
            return native_index[name]
        if COMMAND_RE.match(name):
            # a command not listed yet: add it to the table
            native_index[name] = len(natives)
            natives.append(command_hash(name))
            return native_index[name]
        raise SourceError("line %d: bad command %s" % (no, name))

    def resolve(no, token):
        """A number, a string's address (adding the string to the data when it's new) or a data label's address."""
        if token.startswith('"'):
            text = unquote(token)
            if text not in string_at:
                string_at[text] = len(data) * CELL
                data.extend(ord(ch) for ch in text)
                data.append(0)
            return string_at[text]
        if token in data_labels:
            return data_labels[token]
        try:
            return number(token)
        except ValueError:
            raise SourceError("line %d: %s is not a number, a string or a data label" % (no, token)) from None

    clash = set(labels) & set(data_labels)
    if clash:
        raise SourceError("labels in both code and data: %s" % ", ".join(sorted(clash)))
    for no, at, op, args in layout:
        name, kind = OPCODES[op]
        if kind == "t":
            _, default, cases = args
            cells = [op, len(cases)]
            here = at + CELL * 2
            cells.append((target(no, default) - here + CELL) & MASK)
            for k, (value, lab) in enumerate(cases):
                cells.append(value & MASK)
                here = at + CELL * (4 + 2 * k)
                cells.append((target(no, lab) - here + CELL) & MASK)
            code.extend(cells)
            continue
        if kind == "p":
            if len(args) != 1:
                raise SourceError("line %d: %s takes one number" % (no, name))
            code.append(op | ((resolve(no, args[0]) & 0xFFFFFFFF) << 32))
            continue
        n = 0 if kind == "p" else operands(kind)
        if len(args) != n:
            raise SourceError("line %d: %s takes %d operand(s)" % (no, name, n))
        code.append(op)
        if op in JUMPS:
            code.append((target(no, args[0]) - at) & MASK)
        elif kind == "n":
            code.append(native(no, args[0]))
        elif kind == "ns":
            code.append(native(no, args[0]))
            code.append(number(args[1]) & MASK)
        else:
            code.extend(resolve(no, a) & MASK for a in args)
    # The game writes the event into g_mode, a public variable at the start of the data: a string there (a script
    # with no .data of its own gets its strings at address 0) would be overwritten and read as the event.
    for value, name in pubvars:
        if value >= len(data) * CELL:
            raise SourceError(".pubvar %#010x: data address %#x is past the end of the data" % (name, value))
        for text, start in string_at.items():
            if start <= value < start + CELL * (len(text) + 1):
                raise SourceError(".pubvar %#010x: data address %#x is inside the string %s; start .data with a cell for "
                                  "each public variable (the game's scripts have .cell 0 0 99 99)" % (name, value, quote(text)))
    amx = Amx.__new__(Amx)
    amx.overlays = h.pop("overlays")
    amx.header = {f: h.get(f, 0) for f in FIELDS}
    amx.header["cip"] = labels.get("main", -1)
    for n, _ in publics:
        if n not in labels:
            raise SourceError(".public %s: no such label" % n)
    amx.publics = [(labels[n], hv) for n, hv in publics]
    amx.natives = [(0, hv) for hv in natives]
    amx.libraries, amx.pubvars, amx.tags = libraries, pubvars, tags
    amx.nametable = nametable
    amx.code, amx.data, amx.stack = code, data, stack
    return amx.build()


# --- the field scripts ----------------------------------------------------------------------------------------------

def fnv1a64(text):
    h = 0xCBF29CE484222645
    for c in text.encode():
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def field_scripts():
    """{name: bytes} of the field scripts in orig/romfs (bin/archive/script_amx.gfpak), named where the game names them."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import gfpak
    pak = os.path.join(ROOT, "orig", "romfs", "bin", "archive", "script_amx.gfpak")
    entries = gfpak.read(pak)
    names = {}
    record = os.path.join(ROOT, "orig", "romfs", "bin", "script", "param", "script_id", "script_id_record.bin")
    if os.path.exists(record):
        raw = open(record, "rb").read()
        for m in re.finditer(rb"[a-z0-9_/]+\.amx", raw):
            path = m.group().decode()
            base = os.path.basename(path)
            # each zone's script, and the one it runs when the player enters (init_<zone>)
            for name in (base, "init_" + base):
                names[fnv1a64("bin/script/amx/" + name)] = name
    # every zone's own pair (0x71001a1910 loads "bin/script/amx/%s%s.amx" with "" and "init_" before the zone's name)
    zones = os.path.join(ROOT, "orig", "romfs", "bin", "field", "param", "placement", "table", "ZoneNameHashTable.tbl")
    if os.path.exists(zones):
        raw = open(zones, "rb").read()
        at = 8
        for _ in range(struct.unpack_from("<I", raw, 4)[0] if raw[:4] == b"AHTB" else 0):
            size = struct.unpack_from("<H", raw, at + 8)[0]
            zone = raw[at + 10:at + 9 + size].decode("utf-8", "replace")
            at += 10 + size
            for name in (zone + ".amx", "init_" + zone + ".amx"):
                names.setdefault(fnv1a64("bin/script/amx/" + name), name)
    out = {}
    for e in entries:
        key = e["hash"]
        out[names.get(key, "%016x.amx" % key)] = gfpak.data(e)
    return out


def game_scripts():
    """[(name, bytes)] of every script in orig/romfs: the field scripts, then the battle AI's."""
    files = sorted(field_scripts().items())
    ai = os.path.join(ROOT, "orig", "romfs", "bin", "battle", "ai_script")
    if os.path.isdir(ai):
        files += [(f, open(os.path.join(ai, f), "rb").read()) for f in sorted(os.listdir(ai))]
    return files


def command_uses(files):
    """{hash: {"calls", "scripts", "args": {argument count: calls}}} from every command call in files."""
    out = {}
    for _, data in files:
        a = Amx(data)
        seen = set()
        for _, op, ops in decode(a.code):
            if OPCODES[op][0] != "sysreq.n" or ops[0] >= len(a.natives):
                continue
            h = a.natives[ops[0]][1]
            use = out.setdefault(h, {"calls": 0, "scripts": 0, "args": {}})
            use["calls"] += 1
            use["args"][ops[1] // CELL] = use["args"].get(ops[1] // CELL, 0) + 1
            if h not in seen:
                seen.add(h)
                use["scripts"] += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("file")
    p = sub.add_parser("disasm")
    p.add_argument("file")
    p.add_argument("-o")
    p = sub.add_parser("asm")
    p.add_argument("file")
    p.add_argument("-o", required=True)
    p = sub.add_parser("extract")
    p.add_argument("outdir", nargs="?", default=os.path.join(ROOT, "build", "scripts"))
    p = sub.add_parser("roundtrip")
    p.add_argument("paths", nargs="*")
    p = sub.add_parser("commands")
    p.add_argument("--uses", action="store_true", help="also count each command's calls and arguments in the game's scripts")
    p.add_argument("--ai", action="store_true", help="list the battle AI's AI_CMD subcommands instead")
    a = ap.parse_args()
    if a.cmd == "commands" and a.ai:
        for n, (name, args, what) in sorted(AI_COMMANDS.items()):
            print(("%3d  %-28s %-32s %s" % (n, name, args, what)).rstrip())
        return
    if a.cmd == "commands":
        uses = command_uses(game_scripts()) if a.uses else {}
        for k, v in sorted(all_commands().items(), key=lambda kv: (kv[1]["group"], kv[1]["name"])):
            line = "%08x  %-10s %-40s %-12s" % (k, v["group"], v["name"], v["handler"])
            if a.uses:
                u = uses.get(k)
                args = "/".join("%d" % n for n in sorted(u["args"])) if u else "-"
                line += " args %-6s calls %-6d scripts %d" % (args, u["calls"] if u else 0, u["scripts"] if u else 0)
            print(line.rstrip())
        return
    if a.cmd == "info":
        amx = Amx(open(a.file, "rb").read())
        print(json.dumps({k: (hex(v) if k not in ("file_version", "amx_version", "defsize") else v) for k, v in amx.header.items()}, indent=1))
        names = all_commands()
        print("natives:", ", ".join(names.get(hv, {}).get("name", "%08x" % hv) for _, hv in amx.natives))
        return
    if a.cmd == "disasm":
        text = ai_notes(disassemble(Amx(open(a.file, "rb").read()), all_commands()))
        if a.o:
            open(a.o, "w", encoding="utf-8", newline="\n").write(text)
        else:
            sys.stdout.buffer.write(text.encode("utf-8"))
        return
    if a.cmd == "asm":
        try:
            data = assemble(open(a.file, encoding="utf-8").read(), all_commands())
        except SourceError as e:
            sys.exit("%s: %s" % (a.file, e))
        open(a.o, "wb").write(data)
        return
    if a.cmd == "extract":
        os.makedirs(a.outdir, exist_ok=True)
        files = field_scripts()
        for name, data in files.items():
            open(os.path.join(a.outdir, name), "wb").write(data)
        named = sum(1 for n in files if not re.match(r"^[0-9a-f]{16}\.amx$", n))
        print("%d scripts (%d named) in %s" % (len(files), named, a.outdir))
        return
    if a.cmd == "roundtrip":
        files = [] if a.paths else game_scripts()
        for p in a.paths:
            if os.path.isdir(p):
                files += [(f, open(os.path.join(p, f), "rb").read()) for f in sorted(os.listdir(p))]
            else:
                files.append((os.path.basename(p), open(p, "rb").read()))
        names = all_commands()
        same = 0
        for name, data in files:
            try:
                amx = Amx(data)
                rebuilt = amx.build()
                again = assemble(ai_notes(disassemble(amx, names)), names)
            except (ValueError, SourceError) as e:
                print("%s: %s" % (name, e))
                continue
            if rebuilt != data:
                print("%s: rebuilding the file changes it" % name)
            elif again != data:
                print("%s: disassembling and assembling changes it" % name)
            else:
                same += 1
        print("%d of %d scripts come back byte for byte" % (same, len(files)))


if __name__ == "__main__":
    main()
