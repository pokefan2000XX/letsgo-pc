"""Makes the new-cutscene hack's files for the game (roadmap phase 6): a cutscene of its own, started by a trigger of its
own in the hero's bedroom. From your own copy of the game it writes, in hacks/new-cutscene/romfs/ (which stays on your
PC, since two of the files are made from the game's):

- bin/script/amx/pokefan_cutscene.amx: the script, assembled from pokefan_cutscene.pasm;
- bin/message/English/script/pokefan_cutscene.dat and .tbl: its lines, built from pokefan_cutscene.txt;
- bin/script/param/script_id/script_id_record.bin: the game's event table with the event POKEFAN_CUTSCENE_01 added,
  which names the script and its message file;
- bin/field/param/placement/t001r01.bin: the bedroom's placement file (in placement.gfpak) with a box trigger added to
  zone t001r0102, east of the rug, that starts the event whenever the player is in it.

The script calls the commands of hacks/new-script-command, so the exe needs that hack (LETSGO_HACKS).

  python -P hacks/new-cutscene/make_romfs.py
  build/pc/letsgo.exe --mods hacks/new-cutscene

Needs orig/romfs (tools/prepare.sh).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import amx  # noqa: E402
import flatbuf  # noqa: E402
import gfpak  # noqa: E402
import msg  # noqa: E402

NAME = "pokefan_cutscene"
EVENT = "POKEFAN_CUTSCENE_01"
ZONE = "t001r0102"
CENTRE = (6650.0, 573.0)     # the box's middle (x, z in cm): east of the rug, a step right of where the save stands
SIZE = (200.0, 100.0, 300.0)  # its size (x, y, z), as the game's box triggers give it
RECORDS = "bin/script/param/script_id/script_id_record.bin"
PLACEMENT = "bin/field/param/placement/t001r01.bin"
PLACEMENTS = "bin/archive/field/placement.gfpak"
NO_EVENT = msg.fnv1a64(b"")   # also "no flag" and "no condition"

# The tables this changes (the studio's PlacementSchema has the rest of the file).
TRANSFORM = {f: "f32" for f in range(9)}                       # position, rotation in degrees, scale
TRANSFORM.update({9: "u64", 10: "u64", 11: "u64"})             # its own name's hash, the flag that hides it, its event
TRIGGER = {0: ("table", TRANSFORM), 1: "i32", 2: ("table", TRANSFORM), 3: "i32", 4: "u64", 5: "i32", 6: "u8", 7: "u8"}
TRIGGER_ENTRY = {0: ("table", TRIGGER)}
RECORD_TABLE = {0: ("tables", {0: "u64", 1: "string", 2: "string"})}


def fnv(text):
    return msg.fnv1a64(text.encode())


def orig(path):
    return open(os.path.join(ROOT, "orig", "romfs", *path.split("/")), "rb").read()


def write(path, data, here=HERE):
    out = os.path.join(here, "romfs", *path.split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data)
    print("wrote", os.path.relpath(out, ROOT))


def trigger(event, name=NAME):
    """A box trigger that runs the event whenever the player is in it: shape 2 (a box, its size the shape's scale), and
    the condition "work NO_EVENT equals 0", which always holds. The game's story triggers name a scene work and the
    value it must have; their scripts change it, so they run once."""
    where = {0: ("f32", CENTRE[0]), 2: ("f32", CENTRE[1]), 6: ("f32", 1.0), 7: ("f32", 1.0), 8: ("f32", 1.0),
             9: ("u64", fnv(ZONE + "_" + name)), 10: ("u64", NO_EVENT), 11: ("u64", event)}
    shape = {6: ("f32", SIZE[0]), 7: ("f32", SIZE[1]), 8: ("f32", SIZE[2]), 10: ("u64", NO_EVENT), 11: ("u64", NO_EVENT)}
    return {0: ("table", {0: ("table", where), 1: ("i32", 2), 2: ("table", shape), 3: ("f32", 1.0), 4: ("u64", NO_EVENT)})}


def main(here=HERE, name=NAME, event_name=EVENT):
    """Writes the hack's files into here/romfs: the script and lines named `name` (from here/<name>.pasm and .txt), the
    event's record and the trigger. hacks/new-camera builds its own cutscene with it."""
    event = fnv(event_name)
    source = open(os.path.join(here, name + ".pasm"), encoding="utf-8").read()
    if "%#018x" % event not in source:
        raise SystemExit("make_romfs: %s.pasm doesn't switch on %s's hash, %#018x" % (name, event_name, event))
    try:
        write("bin/script/amx/%s.amx" % name, amx.assemble(source, amx.all_commands()), here)
        dat, tbl = msg.build(open(os.path.join(here, name + ".txt"), encoding="utf-8").read())
    except (amx.SourceError, msg.MessageError) as e:
        raise SystemExit("make_romfs: %s" % e)
    write("bin/message/English/script/%s.dat" % name, dat, here)
    write("bin/message/English/script/%s.tbl" % name, tbl, here)

    # The event table: the event, the script and the message file, after the game's 1,395 records.
    table = flatbuf.copy(flatbuf.root(orig(RECORDS)), RECORD_TABLE)
    records = table[0][1]
    if any(r[0][1] == event for r in records):
        raise SystemExit("make_romfs: the game's event table has %s already" % event_name)
    records.append({0: ("u64", event), 1: ("string", "bin/script/amx/%s.amx" % name), 2: ("string", "script/%s.dat" % name)})
    write(RECORDS, flatbuf.build(table), here)

    # The placement file: the zone's list of triggers, written again after the file's end with the new one, and the
    # zone pointed at it. The rest of the file stays as it is.
    pak = gfpak.read(os.path.join(ROOT, "orig", "romfs", *PLACEMENTS.split("/")))
    entry = next((e for e in pak if e["hash"] == fnv(PLACEMENT)), None)
    if entry is None:
        raise SystemExit("make_romfs: %s has no %s" % (PLACEMENTS, PLACEMENT))
    data = gfpak.data(entry)
    zone = next((z for z in flatbuf.root(data).tables(0) if z.table(0).scalar(1, "u64") == fnv(ZONE)), None)
    if zone is None or zone.slot(14) is None:
        raise SystemExit("make_romfs: %s has no zone %s with a list of triggers" % (PLACEMENT, ZONE))
    triggers = [flatbuf.copy(t, TRIGGER_ENTRY) for t in zone.tables(14)] + [trigger(event, name)]
    w = flatbuf.Writer(data)
    w.point(zone.slot(14), w.tables(triggers))
    w.pad(8)
    write(PLACEMENT, bytes(w.buf), here)


if __name__ == "__main__":
    main()
