# decomp/

Readable C++ that replaces the game's code one function at a time. It is compiled for the
Switch's CPU with the original's ABI and struct layouts, and recompiled with the rest of the game,
so each decompiled function takes the original's place while everything else keeps running.
The order of the work is the decomp roadmap in [docs/PLAN.md](../docs/PLAN.md).

| Path | Contents |
|---|---|
| `include/decomp.h` | Basic types (`u8`…`s64`, `f32`) and layout checks (`DECOMP_OFFSET`, `DECOMP_SIZE`) |
| `include/<area>/`, `src/<area>/` | Headers and code, one folder per part of the game (`pokemon/`) or library (`gfl/`: Game Freak's gflib3) |
| `symbols.txt` | Names of original functions and data by address |
| `tests/<area>/test_*.py` | Differential tests; other `.py` files there are their helpers |
| `tools/build.py` | Builds this folder into the game (the main build runs it) |
| `tools/difftest.py` | Runs every test against the originals and the decompiled versions |
| `tools/progress.py` | Counts what's decompiled, overall and per roadmap phase |

## Decompiling a function

1. **Read the original.** `python -P tools/dis.py <address>` prints it with the names of what it
   calls and the strings it uses; `python -P tools/refs.py <address>` lists who references an
   address (after `python -P tools/refs.py build`). Ghidra's view is in `orig/ghidra/letsgo`.
   All of this comes from the game: it stays in the terminal and `orig/`.
2. **Write it** in `src/<area>/`, with its types in `include/<area>/`. Put the original's address
   in a comment above the definition.
3. **Name it** in `symbols.txt`: the address and the C++ name (`pokemon::Accessor::IsBadEgg`). That
   makes it replace the original. Everything it calls or reads that isn't decompiled yet is
   named there too, so it links to the original.
4. **Test it** in `tests/<area>/`: call the function on many inputs and `t.check()` everything it
   returns or changes. `python -P decomp/tools/difftest.py` runs each test against the
   originals, then against the decompiled code, and the two must match exactly.
5. **Build and run the game:** `cmake --build build/pc`. `LETSGO_DECOMP_TRACE=1` logs each
   replacement's first call; `LETSGO_NO_DECOMP=<address>,...` switches some back to the original.

## Rules

- **Behave exactly like the original:** same results, same memory changes, same calls in the
  same order. Matching the original's instructions isn't the goal.
  - Keep integer widths and truncations: `u16 x = base * 2 + iv` when the original truncates.
  - Keep the order of float operations. The build doesn't fuse multiplies and adds, so write
    `fma` where the original has a fused instruction.
  - Read a parameter's type from how the original uses it. A value it masks to 8 bits is a `u8`;
    a value it compares in full is 32-bit.
- **Nothing copied from the game.** Its strings, tables and other data are declared `extern` and
  named in `symbols.txt`, so the code uses them where they are in the game. The code itself is
  written from what the original does.
- **Replaced functions are defined out of line** in a `.cpp`; inline helpers in headers are fine
  for everything else.
- **Classes with virtual functions** get their own vtable here, so a class's constructors,
  destructors and virtual functions are decompiled together.
- **Names** are our own, chosen for what the code does. PKHeX's names are used for the Pokémon
  record's fields where the game agrees. Unknown fields are `unkXX`, named for their offset.
- **Tests** check everything a function can change (`Record.snapshot()` in
  `tests/pokemon/pkm.py`) and mask return values to their type. Code the test doesn't cover can
  be faked: `emu.stub_function(address, name, handler)` replaces an original function, and
  `emu.host_function(name, handler)` makes a callable function for fake objects. Both runs see
  the same fakes, and their calls are compared too.
