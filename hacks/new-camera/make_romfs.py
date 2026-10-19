"""Makes the new-camera hack's files for the game (roadmap phase 7's check): an edited and a new camera sequence, played in a
cutscene. The cutscene is hacks/new-cutscene's, with an event, script and lines of its own (pokefan_camera.pasm and .txt): its
trigger in the hero's bedroom plays two of the game's demos, and make_romfs.py changes their cameras:

- Bill's teleporter demo (ev0330_masaki) gets a camera of its own, made from keys: it circles the point the demo's own
  cameras look at, from the open side of the room, always facing it. It goes into a new file, pokefan_cam.gfbcama, and the
  demo's sequence (bin/demo/sequence/ev0330_masaki.bseq) is changed to name it instead of its camera animations.
- Zapdos's demo (sub_048_thunder) keeps its camera with the field of view made 1.5 times wider all the way through.

The camera files go through the studio's camera tools (`pokebdsp lgpe export-camera` and `import-camera`, JSON both ways), so
this needs letsgostudio built: give its pokebdsp.exe with --studio or POKEBDSP, or have letsgostudio beside this repo, built in
Release (its own .NET from tools/workspace/dotnet is used when DOTNET_ROOT isn't set). The files go in
hacks/new-camera/romfs/, which stays on your PC (they're made from the game's). The script calls
hacks/new-script-command's commands, so the exe needs that hack.

  python -P hacks/new-camera/make_romfs.py [--studio <pokebdsp.exe>]
  build/pc/letsgo.exe --mods hacks/new-camera

Needs orig/romfs (tools/prepare.sh).
"""
import json
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "hacks", "new-cutscene"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_romfs as cutscene  # noqa: E402
import studio  # noqa: E402

NAME, EVENT = "pokefan_camera", "POKEFAN_CAMERA_01"
BILL_SEQUENCE = "bin/demo/sequence/ev0330_masaki.bseq"
BILL_CAMERAS = ("bin/demo/res/ev0330_masaki/camera/ev0330_cam_motion01_p.gfbcama",
                "bin/demo/res/ev0330_masaki/camera/ev0330_cam_motion01_e.gfbcama")
NEW_CAMERA = "bin/demo/res/ev0330_masaki/camera/pokefan_cam.gfbcama"
ZAPDOS_CAMERA = "bin/demo/res/sub_048_thunder/camera/sub_048_cam_motion01.gfbcama"
WIDER = 1.5
# Where the cameras of Bill's demo look (cm): the point nearest to all their view rays, each frame's camera looking down its
# -z. The new camera circles it on the side of the room where those cameras stand (x 450 to 1400, z 300 to 1350).
FOCUS = (696.0, 96.0, 442.0)
FRAMES = 1354   # as long as the demo


def new_camera():
    """The new camera as JSON keys: every 30 frames a point on an arc around FOCUS (from 25 degrees left of +z to 55
    right, coming closer halfway and rising), facing a little above FOCUS. The import turns the keys into a value a frame."""
    positions, rotations = [], []
    at = [FOCUS[0], FOCUS[1] + 20, FOCUS[2]]
    for frame in list(range(0, FRAMES - 1, 30)) + [FRAMES - 1]:
        t = frame / (FRAMES - 1)
        angle = math.radians(-25 + 80 * t)
        radius = 550 - 130 * math.sin(math.pi * t)
        height = 150 + 80 * t
        positions.append([frame, [round(FOCUS[0] + radius * math.sin(angle), 2), round(FOCUS[1] + height, 2),
                                  round(FOCUS[2] + radius * math.cos(angle), 2)]])
        rotations.append([frame, {"lookAt": at}])
    return {"frames": FRAMES, "position": {"keys": positions}, "rotation": {"keys": rotations}, "fieldOfView": {"value": 0.42}}


def wider(camera):
    """The camera's JSON with its field of view WIDER times as wide, at every frame."""
    fov = camera["fieldOfView"]
    if "value" in fov:
        fov["value"] *= WIDER
    elif "perFrame" in fov:
        fov["perFrame"] = [v * WIDER for v in fov["perFrame"]]
    else:
        fov["keys"] = [[f, v * WIDER] for f, v in fov["keys"]]
    return camera


def patch_sequence(data, old, new):
    """A sequence keeps each file's path in a field of 256 bytes, zero-padded: the new path goes in the old one's place."""
    old_bytes, new_bytes = ("data:/" + old).encode(), ("data:/" + new).encode()
    at = data.find(old_bytes + b"\0")
    if at < 0 or data.find(old_bytes + b"\0", at + 1) >= 0:
        raise SystemExit("make_romfs: %s doesn't name %s once" % (BILL_SEQUENCE, old))
    if len(new_bytes) > len(old_bytes):
        raise SystemExit("make_romfs: %s is longer than the path it replaces" % new)
    data[at:at + len(old_bytes)] = new_bytes + bytes(len(old_bytes) - len(new_bytes))


def main():
    tool = studio.find(studio.given(sys.argv[1:]))
    cutscene.main(here=HERE, name=NAME, event_name=EVENT)
    source = os.path.join(ROOT, "orig")
    for path in (NEW_CAMERA, ZAPDOS_CAMERA):
        old = os.path.join(HERE, "romfs", *path.split("/"))
        if os.path.exists(old):
            os.remove(old)   # so the import builds on the game's file, not on what an earlier run wrote
    with tempfile.TemporaryDirectory() as scratch:
        # The edited camera: Zapdos's demo, through JSON, with a wider view.
        exported = os.path.join(scratch, "zapdos.json")
        studio.run(tool, "export-camera", "--source", source, "--path", ZAPDOS_CAMERA, "--output", exported)
        edited = os.path.join(scratch, "zapdos_wider.json")
        json.dump(wider(json.load(open(exported, encoding="utf-8"))), open(edited, "w", encoding="utf-8"))
        studio.run(tool, "import-camera", "--source", source, "--file", edited, "--path", ZAPDOS_CAMERA, "--output", HERE)
        print("wrote", os.path.relpath(os.path.join(HERE, "romfs", *ZAPDOS_CAMERA.split("/")), ROOT))
        # The new camera: Bill's demo, from keys, on the demo's own camera for the lens and the rest.
        keys = os.path.join(scratch, "new.json")
        json.dump(new_camera(), open(keys, "w", encoding="utf-8"))
        studio.run(tool, "import-camera", "--source", source, "--file", keys, "--path", NEW_CAMERA, "--like", BILL_CAMERAS[0], "--output", HERE)
        print("wrote", os.path.relpath(os.path.join(HERE, "romfs", *NEW_CAMERA.split("/")), ROOT))
    sequence = bytearray(cutscene.orig(BILL_SEQUENCE))
    for old in BILL_CAMERAS:
        patch_sequence(sequence, old, NEW_CAMERA)
    cutscene.write(BILL_SEQUENCE, bytes(sequence), HERE)


if __name__ == "__main__":
    main()
