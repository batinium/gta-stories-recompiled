#!/usr/bin/env python3
"""[android] "Prepare for Android": your disc -> the game library for the phone, then the file to copy there.

    python tools/android_pack.py <your .iso> [--game vcs|lcs] [--out android-export]

What the PC launcher's "Prepare for Android" button runs (the same steps as tools/build-android-pack.bat):
  1. preflight  : the tools the build needs (tools/preflight.py: all that is missing, with how to get each) and the
                  Windows path length, before any long step. Something missing: in a terminal it runs the fetch
                  (tools/toolchain_fetch.py, the same as `tools/android.py setup`: lists the downloads, asks y/N);
                  for the launcher (VCS_PROGRESS=json) it never downloads on its own: error code android_tools,
                  whose message starts with the setup command
  2. recompile  : tools/recompile.py --game <game> <iso>
  3. build      : tools/android.py build-game --game <game>
  4. export     : build/android/game[-lcs]/libvcs_game.so.gz -> <out>/<game>/ (default android-export/, untracked)
Prints `@vcs` events for the launcher (tools/toolchain.py): stages, an error code + message, and the result.
The library holds game code from your disc: for your own phone only, never share it.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import preflight  # noqa: E402
import toolchain_fetch  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
EVENT_PREFIX = "@vcs "


def event(**fields):
    import json
    if os.environ.get("VCS_PROGRESS") == "json":
        print(EVENT_PREFIX + json.dumps(fields), flush=True)


def stage(stage_id, title):
    print("[pack] " + title, flush=True)
    event(stage=stage_id, title=title)


def fail(message, code="python_failed"):
    event(error=code, message=message)
    print("error: " + message, file=sys.stderr, flush=True)
    sys.exit(1)


def run(args, what, code, base=0, span=100):
    """One step's own script: its output is the log. Its progress becomes this script's one bar (`base` .. `base+span`
    of 100): the step's own `@vcs` progress events when it sends them (the recompile does), else the [n/m] of Ninja's
    lines (the NDK build). Its stage / result events are dropped (this script's stages are the timeline); its error
    event's message is kept, to say why it failed."""
    import json
    import re
    env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONUTF8="1")
    proc = subprocess.Popen([sys.executable] + args, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, encoding="utf-8", errors="replace")
    last_error, own = None, False
    event(progress=int(base), total=100, eta=None, note="")

    def bar(done, total, eta=None, note=""):
        if total > 0:
            event(progress=int(base + span * min(1.0, done / total)), total=100, eta=eta, note=note)

    for line in proc.stdout:
        line = line.rstrip("\r\n")
        if line.startswith(EVENT_PREFIX):
            try:
                ev = json.loads(line[len(EVENT_PREFIX):])
            except ValueError:
                continue
            if "progress" in ev and ev.get("total"):
                own = True
                bar(ev["progress"], ev["total"], ev.get("eta"), ev.get("note") or "")
            elif ev.get("error"):
                last_error = ev.get("message")
            continue
        print(line, flush=True)
        if not own:
            m = re.match(r"^\[(\d+)/(\d+)\]", line)
            if m:
                bar(int(m.group(1)), int(m.group(2)))
    if proc.wait() != 0:
        fail(("%s failed: %s" % (what, last_error)) if last_error else "%s failed (see the log)" % what, code)
    event(progress=int(base + span), total=100, eta=None, note="")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", help="your Vice City Stories or Liberty City Stories (USA) disc image")
    ap.add_argument("--game", choices=("vcs", "lcs"), help="default: the game the disc is")
    ap.add_argument("--out", default=os.path.join(ROOT, "android-export"), help="the export folder")
    ap.add_argument("--no-setup", action="store_true", help="only report missing tools, never offer to fetch them")
    a = ap.parse_args()
    toolchain_fetch.apply_env()  # a fetched toolchain: its JDK / glslang / MinGit for the steps below

    stage("preflight", "Checking this PC")
    if not os.path.isfile(a.iso):
        fail("the disc image was not found: " + a.iso, "disc_not_found")
    game, error = preflight.resolve_game(a.iso, a.game)
    if error:
        fail(error, "wrong_disc")
    if game not in ("vcs", "lcs"):
        fail("this disc is not Vice City Stories or Liberty City Stories (USA)", "wrong_disc")
    host = preflight.Host()
    problems = preflight.missing_tools(host, root=ROOT)
    if problems and not a.no_setup and toolchain_fetch.is_interactive():
        # a terminal: offer the fetch now (it asks first), then check again
        print(preflight.format_problems(problems, host), flush=True)
        stage("setup", "Fetching the missing tools")
        toolchain_fetch.setup(host, root=ROOT)
        toolchain_fetch.apply_env()
        host = preflight.Host()
        problems = preflight.missing_tools(host, root=ROOT)
    if problems:
        fail(preflight.format_problems(problems, host), "android_tools")
    problem = preflight.path_problem(ROOT, "build-game")
    if problem:
        fail(problem, "path_too_long")
    print("[pack] %s, tools found, path length fine" % preflight.GAME_NAME[game], flush=True)

    stage("recompile", "Recompiling the game from your disc")
    run([os.path.join(TOOLS, "recompile.py"), "--game", game, a.iso], "The recompile step", "recompiler_failed", 0, 25)

    stage("build", "Building the game library with the Android NDK")
    run([os.path.join(TOOLS, "android.py"), "build-game", "--game", game], "The build step", "build_failed", 25, 73)

    stage("export", "Copying the file to export")
    event(progress=98, total=100, eta=None, note="")
    built = os.path.join(ROOT, "build", "android", "game" if game == "vcs" else "game-" + game, "libvcs_game.so.gz")
    if not os.path.isfile(built):
        fail("the build made no " + built, "build_failed")
    folder = os.path.join(os.path.abspath(a.out), game)
    os.makedirs(folder, exist_ok=True)
    target = os.path.join(folder, "libvcs_game.so.gz")
    shutil.copyfile(built, target)
    size = os.path.getsize(target)
    print("[pack] exported: %s (%d MB)" % (target, size >> 20), flush=True)
    event(result="ok", game=game, folder=folder.replace(os.sep, "/"), file=target.replace(os.sep, "/"), size=size)


if __name__ == "__main__":
    main()
