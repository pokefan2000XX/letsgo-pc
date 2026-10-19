# phase1-test

The check for the roadmap's phase 1 (`docs/PLAN.md`): a hack that uses the archives, the mods
folder and hooks.

- `code/file_log.cpp` hooks the game's file open (`0x71000238e0`): it logs each file before the
  original opens it, which it calls through `ORIG_71000238e0`. `tests/test_file_log.py` checks
  that the hooked function still opens files as the original does.
- `make_romfs.py` makes its files from your copy of the game (`orig/romfs`): the rugs in the
  player's bedroom, which are inside the bedroom's gfpak, and the hero's cap, a file of its own,
  all recolored blue. They go in `romfs/`, which stays on your PC.

```bash
python -P hacks/phase1-test/make_romfs.py
cmake -DLETSGO_HACKS=phase1-test build/pc
cmake --build build/pc --target letsgo
build/pc/letsgo.exe --mods hacks/phase1-test
```

In the bedroom the rug and the cap are blue, and `letsgo.log` has a `phase1-test: open` line for
every file the game opens. `cmake -DLETSGO_HACKS= build/pc` builds the hack out again.
