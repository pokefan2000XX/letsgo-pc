"""Makes the wild-battles hack's file for the game from your own copy of it: the battle bag's item table
(bin/pokelib/item/battle_pocket.dat, one byte per item: 0 not in the battle bag, 1 medicine, 2 battle items)
with the Poké Balls in the medicine list, so a wild battle's bag offers them. It goes in
hacks/wild-battles/romfs/, which stays on your PC (it's made from the game).

  python -P hacks/wild-battles/make_romfs.py
  build/pc/letsgo.exe --mods hacks/wild-battles

Needs orig/romfs (tools/prepare.sh).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = "bin/pokelib/item/battle_pocket.dat"
BALLS = (1, 2, 3, 4, 12)   # Master, Ultra, Great, Poké and Premier Ball: Let's Go's balls
MEDICINE = 1


def main():
    data = bytearray(open(os.path.join(ROOT, "orig", "romfs", *TABLE.split("/")), "rb").read())
    if len(data) <= max(BALLS) or any(data[ball] not in (0, MEDICINE) for ball in BALLS):
        raise SystemExit("make_romfs: %s isn't the table this hack expects" % TABLE)
    for ball in BALLS:
        data[ball] = MEDICINE
    out = os.path.join(ROOT, "hacks", "wild-battles", "romfs", *TABLE.split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data)
    print("wrote", os.path.relpath(out, ROOT))


if __name__ == "__main__":
    main()
