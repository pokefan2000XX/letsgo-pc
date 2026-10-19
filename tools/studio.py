"""letsgostudio's command line (pokebdsp.exe) for the hacks' scripts: the studio has the format tools for textures, models
and cameras, and hacks/new-camera and hacks/menu-edit call it.

The studio is found from --studio, else POKEBDSP, else letsgostudio beside this repo, built in Release; its own .NET
(letsgostudio/tools/workspace/dotnet) is used when DOTNET_ROOT isn't set.

  import studio
  tool = studio.find(given)                      # given: the --studio path, or None
  result = studio.run(tool, "export", "--file", bntx, "--output", folder)   # pokebdsp lgpe ...: its "result"
"""
import glob
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class StudioError(SystemExit):
    pass


def given(args):
    """The path after --studio in a script's arguments, or None."""
    if "--studio" in args and args.index("--studio") + 1 < len(args):
        return args[args.index("--studio") + 1]
    return None


def find(exe=None):
    exe = exe or os.environ.get("POKEBDSP")
    studio_root = os.path.join(os.path.dirname(ROOT), "letsgostudio")
    if not exe:
        exe = os.path.join(studio_root, "src", "PokeBdsp.Tool", "bin", "Release", "net8.0", "pokebdsp.exe")
    if not os.path.isfile(exe):
        raise StudioError("studio: the studio's pokebdsp.exe isn't at %s; give it with --studio or POKEBDSP" % exe)
    env = dict(os.environ)
    if "DOTNET_ROOT" not in env:
        own = sorted(glob.glob(os.path.join(studio_root, "tools", "workspace", "dotnet", "*", "win-x64")))
        if own:
            env["DOTNET_ROOT"], env["DOTNET_MULTILEVEL_LOOKUP"] = own[-1], "0"
    return exe, env


def run(tool, *args):
    exe, env = tool
    done = subprocess.run([exe, "lgpe", *args], capture_output=True, text=True, env=env)
    try:
        reply = json.loads(done.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError):
        raise StudioError("studio: pokebdsp lgpe %s said nothing it could read:\n%s%s" % (args[0], done.stdout, done.stderr))
    if not reply.get("success"):
        raise StudioError("studio: pokebdsp lgpe %s failed: %s" % (args[0], reply.get("diagnostics")))
    return reply["result"]
