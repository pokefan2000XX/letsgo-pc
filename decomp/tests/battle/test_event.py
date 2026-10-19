"""decomp/src/battle/event.cpp against the originals: handler sets added, removed, put to sleep and looked up
in random orders; events called through made-up handlers that change the list as they run, with and without
skip checks; and the event variables pushed, set, rewritten, multiplied and read."""
import struct

RESET, WAKE_ALL, ADD, FREE_REMOVED, REMOVE = 0x7100A66740, 0x7100A667C0, 0x7100A667F0, 0x7100A66910, 0x7100A669F0
MARK_REMOVED, IS_MARKED, SLEEP, WAKE, CLEAR_WORK = 0x7100A66AA0, 0x7100A66AB0, 0x7100A66AC0, 0x7100A66AE0, 0x7100A66B00
GET_ID, GET_OWNER, GET_WORK, ANSWER_8B, ALLOW_REENTRY = 0x7100A66B20, 0x7100A66B30, 0x7100A66B40, 0x7100A66B50, 0x7100A66B60
SET_WORK, CALL_EVENT, CALL_EVENT_CHECKED, SET_SKIP, CLEAR_SKIP = 0x7100A66B80, 0x7100A66B90, 0x7100A66C70, 0x7100A66D50, 0x7100A66D60
FIRST_SET, NEXT_SET, MOVES_SLEEP, WAKE_MOVES = 0x7100A66D70, 0x7100A66DC0, 0x7100A66E10, 0x7100A66E80
SUPPRESS, UNSUPPRESS = 0x7100A66EC0, 0x7100A66F10
RESET_VARS, PUSH, POP, SET_VAR, SET_CONST, SET_ONCE = 0x7100A66F70, 0x7100A66FA0, 0x7100A67000, 0x7100A67090, 0x7100A67100, 0x7100A67170
MAKE_ONCE, SET_RATIO, REWRITE, MULTIPLY, GET_VAR, FIND_VAR = 0x7100A671E0, 0x7100A67230, 0x7100A672A0, 0x7100A67340, 0x7100A673E0, 0x7100A67450
SET_PTR47, GET_PTR47, SET_PTR23, GET_PTR23 = 0x7100A674C0, 0x7100A67590, 0x7100A67630, 0x7100A67700
CALL_HANDLERS, SHOULD_SKIP = 0x7100A677A0, 0x7100A67990
# what ShouldSkip asks the rest of the battle (faked)
GET_POKEMON, HAS_FIELD_EFFECT, HAS_CONDITION, HAS_FLAG = 0x7100A095E0, 0x7100A0A1E0, 0x71009E3DA0, 0x71009E5390

SETS, USED, DEPTH, FIRST, VARS, FREE = 0x7101649560, 0x710164D75C, 0x710164D760, 0x710164D768, 0x710164D770, 0x710164DD18
SET_SIZE, MAX_SETS, MAX_VARS = 0x50, 211, 96
LABELS, VALUES, MAXES, MINS, MODES = VARS + 4, VARS + 0xC4, VARS + 0x244, VARS + 0x3C4, VARS + 0x544
RUNNING, ASLEEP, ANSWERS_8B, REMOVE_WHEN_DONE = 1 << 24, 1 << 25, 1 << 26, 1 << 27
REENTRANT, IN_USE, SUPPRESSED = 1 << 28, 1 << 29, 1 << 30
U8, U16, U32, U64 = 0xFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
FLOW = 0x7101230000                     # never read: the battle's parts it would give are faked
POKEMON = 0x7101240000                  # the fake Pokémon: POKEMON + 0x100 * id

FAKES = {}                              # the faked battle's answers, set by each test


def setup(emu):
    def get_pokemon(e):
        i = e.reg(1) & U8
        return POKEMON + 0x100 * i if i in FAKES.get("present", ()) else 0

    def has_condition(e):
        return 1 if ((e.reg(0) - POKEMON) // 0x100, e.reg(1) & U32) in FAKES.get("conditions", ()) else 0

    def has_flag(e):
        return 1 if ((e.reg(0) - POKEMON) // 0x100, e.reg(1) & U32) in FAKES.get("flags", ()) else 0

    def has_field_effect(e):
        return 1 if (e.reg(1) & U32) in FAKES.get("field", ()) else 0
    emu.stub_function(GET_POKEMON, "GetPokemon", get_pokemon)
    emu.stub_function(HAS_CONDITION, "HasCondition", has_condition)
    emu.stub_function(HAS_FLAG, "HasFlag", has_flag)
    emu.stub_function(HAS_FIELD_EFFECT, "HasFieldEffect", has_field_effect)


def u32(e, a):
    return struct.unpack("<I", e.read(a, 4))[0]


def u64(e, a):
    return e.read_u64(a)


def w32(e, a, v):
    e.write(a, struct.pack("<I", v & U32))


def w64(e, a, v):
    e.write(a, struct.pack("<Q", v & U64))


def sets_state(e):
    return (e.read(SETS, SET_SIZE * MAX_SETS), u32(e, USED), u32(e, DEPTH), u64(e, FIRST),
            e.read(FREE, 8 * MAX_SETS))


def vars_state(e):
    return e.read(VARS, 0x5A4)


def listed(e):
    """The sets on the list, first to last."""
    out, s = [], u64(e, FIRST)
    while s and len(out) <= MAX_SETS:
        out.append(s)
        s = u64(e, s + 8)
    return out


def py_free(e, s):
    """What RemoveHandlerSet does to a set that isn't running."""
    first, nxt, prv = u64(e, FIRST), u64(e, s + 8), u64(e, s)
    if first == s:
        w64(e, FIRST, nxt)
    if prv:
        w64(e, prv + 8, nxt)
    if nxt:
        w64(e, nxt, prv)
    used = u32(e, USED)
    if used:
        e.write(s, bytes(SET_SIZE))
        w32(e, USED, used - 1)
        w64(e, FREE + 8 * (used - 1), s)


def py_remove(e, s):
    flags = u32(e, s + 0x28)
    if not flags & IN_USE:
        return
    if flags & RUNNING:
        w32(e, s + 0x28, flags | REMOVE_WHEN_DONE)
    else:
        py_free(e, s)


def add(t, kind, handlers, count, sub=None, priority=None, pokemon=None, ident=None):
    r = t.rng
    sub = r.choice([0, 1, 2, 3, 4, 5, 8, 9, 10, 11]) if sub is None else sub
    priority = r.choice([0, 1, 2, 5, 0x10, r.getrandbits(24), r.getrandbits(32)]) if priority is None else priority
    pokemon = r.choice([0, 1, 2, 3, 4, 5, 0x1F, r.getrandbits(8)]) if pokemon is None else pokemon
    ident = r.getrandbits(16) if ident is None else ident
    return t.call(ADD, kind, ident, sub, priority, pokemon, handlers, count)


def test_handler_sets(t):
    e, r = t.emu, t.rng
    t.call(RESET)
    t.check(sets_state(e))
    tables = [e.alloc(struct.pack("<IIQ", r.randrange(0x100), 0, 0x7100001000 + i) * 4) for i in range(3)]
    made = [0]
    for step in range(700):
        op = r.randrange(24)
        pick = r.choice(made)
        if op < 6:
            if r.random() < 0.1:
                w32(e, DEPTH, r.choice([0, 1, 2, 0x10003]))
            kind = r.choice([0, 0, 0, 1, 2, 3, 4, 4, 5, 5, 6, 7, 8, 9, r.getrandbits(32)])
            s = add(t, kind, r.choice(tables), r.choice([0, 1, 2, 3, 4, 0x101, r.getrandbits(32)]))
            t.check(s)
            if s:
                made.append(s)
        elif op < 8 and pick:
            if r.random() < 0.3:          # as though one of its handlers were running
                w32(e, pick + 0x28, u32(e, pick + 0x28) | RUNNING)
            t.call(REMOVE, pick)
        elif op == 8:
            t.call(REMOVE, 0)
        elif op == 9 and pick:
            t.call(MARK_REMOVED, pick)
        elif op == 10:
            t.call(FREE_REMOVED)
        elif op == 11 and pick:
            t.check((t.call(SLEEP, pick) & U8 != 0, t.call(WAKE, pick) & U8 != 0 if r.random() < 0.5 else None))
        elif op == 12:
            t.call(WAKE_ALL)
        elif op == 13:
            owner = r.choice([0, 1, 2, 3, 0x1F, 0x101])
            if r.random() < 0.5:
                t.call(MOVES_SLEEP, owner)
            else:
                t.call(WAKE_MOVES, owner)
        elif op == 14:
            owner, kind = r.choice([0, 1, 2, 3, 0x1F, 0x100]), r.choice([0, 4, 5, 6, 8])
            if r.random() < 0.5:
                t.call(SUPPRESS, owner, kind)
            else:
                t.check(t.call(UNSUPPRESS, owner, kind) & U8 != 0)
        elif op == 15 and pick:
            t.check((t.call(GET_ID, pick) & U16, t.call(GET_OWNER, pick) & U8, t.call(IS_MARKED, pick) & U8 != 0,
                     t.call(GET_WORK, pick, r.choice([0, 3, 6, 0x105, r.randrange(20)])) & U32))
        elif op == 16 and pick:
            t.call(SET_WORK, pick, r.choice([0, 1, 6, 0x106]), r.choice([0, 1, r.getrandbits(32)]))
        elif op == 17 and pick:
            t.call(CLEAR_WORK, pick)
        elif op == 18 and pick:
            if r.random() < 0.5:
                w32(e, pick + 0x28, u32(e, pick + 0x28) ^ RUNNING)
            t.call(r.choice([ANSWER_8B, ALLOW_REENTRY]), pick)
        elif op == 19 and pick:
            if r.random() < 0.5:
                t.call(SET_SKIP, pick, r.getrandbits(64))
            else:
                t.call(CLEAR_SKIP, pick)
        elif op in (20, 21):
            kind, pokemon = r.choice([0, 0, 1, 4, 5, 8]), r.choice([0, 1, 2, 3, 0x1F, 0x101])
            found, s = [], t.call(FIRST_SET, kind, pokemon)
            while s and len(found) < 300:
                found.append(s)
                s = t.call(NEXT_SET, s)
            t.check(found)
        elif op == 22:
            t.check(t.call(NEXT_SET, 0))
        elif op == 23 and r.random() < 0.1:
            t.call(RESET)
            made = [0]
        t.check(sets_state(e))


def test_handler_sets_full(t):
    # every set handed out, then more: the rest come back null; then freed and handed out again
    e, r = t.emu, t.rng
    t.call(RESET)
    table = e.alloc(16)
    made = [add(t, r.randrange(8), table, 1, sub=r.randrange(10), priority=r.randrange(4)) for _ in range(MAX_SETS + 3)]
    t.check(made)
    t.check(sets_state(e))
    for s in r.sample([m for m in made if m], 100):
        t.call(REMOVE, s)
    t.check(sets_state(e))
    for s in made[:50]:
        if s:
            t.call(MARK_REMOVED, s)
    t.call(FREE_REMOVED)
    t.check(sets_state(e))
    t.check([add(t, r.randrange(8), table, 2) for _ in range(40)])
    t.check(sets_state(e))
    # a free slot that holds null: the count still goes up
    w64(e, FREE + 8 * u32(e, USED), 0)
    t.check(add(t, 0, table, 1))
    t.check(sets_state(e))
    w32(e, USED, 0)
    for s in listed(e)[:5]:
        t.call(REMOVE, s)                  # nothing handed out: unlinked, not freed
    t.check(sets_state(e))


class Battle:
    """Made-up handlers for one test: each records how it was called and may change the list as the
    game's handlers do (removing sets, itself included, putting them to sleep, allowing reentry)."""

    def __init__(self, t, events):
        self.t, self.e, self.r = t, t.emu, t.rng
        self.events = events
        self.functions = [self.e.host_function("handler%d" % i, self.handler(i)) for i in range(6)]
        self.checks = [self.e.host_function("skip%d" % i, self.skip(i)) for i in range(3)]

    def handler(self, i):
        def run(e):
            t, r = self.t, self.r
            s, flow, pokemon, work = e.reg(0), e.reg(1), e.reg(2) & U8, e.reg(3)
            t.check(("handler", i, s, flow, pokemon, work - s, u32(e, s + 0x28)))
            w32(e, work, u32(e, work) + 1)
            what = r.randrange(13)
            others = [x for x in listed(e) if x != s]
            if what == 0:
                py_remove(e, s)                               # RemoveHandlerSet on itself: marked
            elif what == 1 and others:
                py_remove(e, u64(e, s + 8) or r.choice(others))   # the next set, freed
            elif what == 2 and others:
                py_remove(e, r.choice(others))
            elif what == 3:
                w32(e, s + 0x28, u32(e, s + 0x28) | REENTRANT)    # AllowReentry
            elif what == 4 and others:
                o = r.choice(others)
                w32(e, o + 0x28, u32(e, o + 0x28) | r.choice([ASLEEP, SUPPRESSED, ANSWERS_8B]))
            elif what == 5 and others:
                o = r.choice(others)
                w32(e, o + 0x20, 8)                                 # MarkRemoved
            elif what == 6:
                py_remove(e, s)
                if others:
                    py_remove(e, r.choice(others))
            elif what == 7:
                py_remove(e, s)                               # itself and the set after it, which
                if u64(e, s + 8):                             # the event then goes past
                    py_remove(e, u64(e, s + 8))
        return run

    def skip(self, i):
        def check(e):
            args = (e.reg(0), e.reg(1), e.reg(2) & U32, e.reg(3) & U32, e.reg(4) & U16, e.reg(5) & U8)
            self.t.check(("skip", i) + args)
            return 1 if self.r.random() < 0.25 else 0
        return check

    def table(self):
        r = self.r
        entries = [(r.choice(self.events), r.choice(self.functions)) for _ in range(r.randrange(1, 4))]
        return self.e.alloc(b"".join(struct.pack("<IIQ", ev, 0, fn) for ev, fn in entries)), len(entries)

    def populate(self, n):
        e, r = self.e, self.r
        made = []
        for _ in range(n):
            handlers, count = self.table()
            s = add(self.t, r.choice([0, 0, 1, 2, 3, 4, 4, 5, 5, 6, 7]), handlers, r.choice([count, count, 0, 0x100 + count]),
                    pokemon=r.choice([0, 1, 2, 3, 0x1F]))
            if not s:
                continue
            made.append(s)
            flags = u32(e, s + 0x28)
            for bit, p in ((ASLEEP, 0.08), (SUPPRESSED, 0.08), (ANSWERS_8B, 0.3), (RUNNING, 0.08), (REENTRANT, 0.05)):
                if r.random() < p:
                    flags |= bit
            if r.random() < 0.2:
                flags = flags & ~0xFFFF | r.choice([1, 2, 3])        # added during an event
            w32(e, s + 0x28, flags)
            if r.random() < 0.3:
                w64(e, s + 0x18, r.choice(self.checks))
        return made


def fake_battle(r):
    FAKES.clear()
    FAKES["present"] = set(r.sample(range(0x20), 12))
    FAKES["conditions"] = {(p, c) for p in range(0x20) for c in (0x10, 0x13) if r.random() < 0.2}
    FAKES["flags"] = {(p, 9) for p in range(0x20) if r.random() < 0.2}
    FAKES["field"] = {7} if r.random() < 0.3 else set()


def test_call_event(t):
    e, r = t.emu, t.rng
    for scene in range(40):
        fake_battle(r)
        t.call(RESET)
        b = Battle(t, [5, 0x47, 0x8B, 0xCB, 0x10])
        b.populate(r.randrange(1, 24))
        for _ in range(4):
            w32(e, DEPTH, r.choice([0, 0, 0, 1, 2]))
            t.call(r.choice([CALL_EVENT, CALL_EVENT_CHECKED]), FLOW, r.choice([5, 0x47, 0x8B, 0xCB, 0x10, 0x99]))
            t.check(sets_state(e))


def test_call_handlers_and_skips(t):
    e, r = t.emu, t.rng
    for scene in range(40):
        fake_battle(r)
        t.call(RESET)
        b = Battle(t, [5, 0x47, 0x8B, 0xCB])
        made = b.populate(r.randrange(1, 16))
        w32(e, DEPTH, r.choice([0, 1, 2, 3]))
        for _ in range(8):
            live = listed(e)
            if not live:
                break
            s = r.choice(live)
            event = r.choice([5, 0x47, 0x8B, 0xCB])
            if r.random() < 0.5:
                t.check(t.call(SHOULD_SKIP, FLOW, s, event) & U8 != 0)
            else:
                t.check(t.call(CALL_HANDLERS, FLOW, event, r.choice([9, 9, 0, 4, 5, u32(e, s + 0x20)]),
                               r.choice([0, 1]), s))
            t.check(sets_state(e))
        t.check(len(made))


LABELS_POOL = [0, 1, 2, 3, 4, 0x12, 0x23, 0x24, 0x37, 0x47, 0x48, 0x10037, 0x8003]


def test_vars(t):
    e, r = t.emu, t.rng
    t.call(RESET)
    out = e.alloc(8)
    for step in range(1500):
        op = r.randrange(20)
        label = r.choice(LABELS_POOL + [r.getrandbits(16)])
        value = r.choice([0, 1, -1, 100, 0x7FFFFFFF, -0x80000000, r.getrandbits(32) - (1 << 31)])
        if op < 2:
            t.call(PUSH)
        elif op < 4:
            t.call(POP)
        elif op == 4 and r.random() < 0.2:
            t.call(RESET_VARS)
        elif op == 5:
            t.call(SET_VAR, label, value)
        elif op == 6:
            t.call(SET_CONST, label, value)
        elif op == 7:
            t.call(SET_ONCE, label, value)
        elif op == 8:
            t.call(MAKE_ONCE, label)
        elif op == 9:
            lo = r.choice([0, 0, -5, 10, r.getrandbits(32) - (1 << 31)])
            hi = r.choice([0, 0, 100, 0x10000, r.getrandbits(32) - (1 << 31)])
            t.call(SET_RATIO, label, value, lo, hi)
        elif op == 10:
            t.check(t.call(REWRITE, label, value) & U8 != 0)
        elif op in (11, 12):
            ratio = r.choice([0x1000, 0x2000, 0x800, 0x5000, 0x1800, 0, -0x1000, r.getrandbits(32) - (1 << 31)])
            t.call(MULTIPLY, label, ratio)
        elif op == 13:
            t.check(t.call(GET_VAR, label) & U32)
        elif op == 14:
            w64(e, out, 0xA5A5A5A5A5A5A5A5)
            t.check((t.call(FIND_VAR, label, out) & U8 != 0, u64(e, out)))
        elif op == 15:
            t.call(r.choice([SET_PTR47, SET_PTR23]), r.getrandbits(64))
        elif op == 16:
            t.check((t.call(GET_PTR47), t.call(GET_PTR23)))
        elif op == 17 and r.random() < 0.3:
            # a frame where the stack's checks stop things: past the end, or right at it
            saved = u32(e, VARS)
            w32(e, VARS, r.choice([0x5E, 0x5F, 0x60, 0xFFFFFFFF]))
            t.call(r.choice([PUSH, SET_VAR, GET_VAR, MULTIPLY, REWRITE, SET_PTR47, GET_PTR47, MAKE_ONCE, RESET_VARS]),
                   label, value)
            t.check(vars_state(e))
            if u32(e, VARS) > 0x60:
                w32(e, VARS, saved)
        elif op == 18:
            # a full frame: Set falls back on slot 0
            for i in range(r.randrange(80, 96)):
                t.call(SET_VAR, 0x100 + i, i)
        elif op == 19 and r.random() < 0.1:
            t.call(RESET)
        t.check(vars_state(e))
