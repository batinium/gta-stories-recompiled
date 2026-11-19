#!/usr/bin/env python3
"""The Stories packs, made by themselves (docs/STORIES.md "The packs, automatically"). Standard library only.

When the launcher knows both discs it runs this before Play: every other-game pack the game being started can use is
made from the player's own disc, or made again when it is missing or out of date, and switched on in the mod list.
A pack that is current costs nothing (a stamp is compared; the disc is not read). Nothing here is shipped: the packs
are the player's own data, in the games' data folders (tools/stories_pack.py's rules).

    python tools/stories_auto.py [--for lcs|vcs|stories] [--lcs-iso X] [--vcs-iso Y] [--only KIND,...] [--force]
                                 [--check] [--json]

--for: the game about to start - lcs (Liberty City Stories alone), vcs (Vice City Stories alone) or stories (both
cities: Liberty City in front, Vice City in the background from its own data folder, <build>/stories/vcs-background,
as tools/stories.py starts it). The discs: --lcs-iso / --vcs-iso, else iso= in each game's vcs.ini (the launcher's
choice). --check only says what would be made. --only limits it to some packs (swim, vehicles, ...); --force makes
them again even when current (the Mods sheet's button).

The packs (PACKS below; tools/stories_pack.py may add its own with a list AUTO_PACKS of the same dicts):
    swim       Vic's swimming animations for LCS   (stories_pack.py swim <VCS iso>)          mod stories_vcs_swim
    vehicles   Vice City's vehicles for LCS         (stories_pack.py build <VCS iso> --game lcs) stories_vcs_vehicles
    vehicles   Liberty City's vehicles for VCS      (stories_pack.py build <LCS iso> --game vcs) stories_lcs_vehicles
               - only once stories_pack.py says it supports that direction: DIRECTIONS containing ("lcs", "vcs"),
               or supports("lcs", "vcs") returning true (tools/stories_pack.py; until then it is left out, said once)

Up to date means: the mod's manifest is there and its stamp (<data>/mods/<mod>/stories_auto.json) matches - this
file's PACK_VERSION, the pack's own version (stories_pack.PACK_VERSION if it has one), a hash of the tools that make
it (stories_pack.py, stories_swim.py, vcs_disc.py, and any `tools` a pack names) and the disc (its path, size and
date). A change to any of them makes the pack again at the next Play. A pack made for the first time is switched on;
one the player turned off in the Mods sheet stays off when it is made again (the stamp remembers it was ours).
A pack that fails is reported (an `@vcs` note and the result's "failed") and the others still go: the game plays
without it. Exit code 0 unless the command line was wrong. VCS_STORIES_AUTO_FAIL=<kind or mod> makes that pack fail (tests).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.pop("VCS_TOOLS_GAME", None)  # the base build folder (tools/stories.py's): build/, build/lcs, build/stories
import toolchain as tc  # noqa: E402

PACK_VERSION = 1  # this file's own: bump to make every pack again
STAMP = "stories_auto.json"

# kind, the game it is for, the disc it reads, its mod id, the stories_pack.py arguments ({iso}, {data}), its title,
# the tools whose text decides whether it is current (besides stories_pack.py and vcs_disc.py)
PACKS = [
    {"kind": "swim", "game": "lcs", "src": "vcs", "mod": "stories_vcs_swim",
     "args": ["swim", "{iso}", "--data", "{data}"], "title": "Vic's swim animations for Liberty City",
     "tools": ["stories_swim.py"]},
    {"kind": "vehicles", "game": "lcs", "src": "vcs", "mod": "stories_vcs_vehicles",
     "args": ["build", "{iso}", "--game", "lcs", "--data", "{data}"],
     "title": "Vice City's vehicles for Liberty City", "tools": []},
    {"kind": "vehicles", "game": "vcs", "src": "lcs", "mod": "stories_lcs_vehicles",
     "args": ["build", "{iso}", "--game", "vcs", "--data", "{data}"],
     "title": "Liberty City's vehicles for Vice City", "tools": [], "needs": ("lcs", "vcs")},
]


def say(**fields):
    tc.event(**fields)


def stories_pack():
    """tools/stories_pack.py as a module (its constants: PACK_VERSION, DIRECTIONS, supports(), AUTO_PACKS), or None"""
    try:
        import stories_pack as sp
        return sp
    except (ImportError, SyntaxError, SystemExit) as e:
        print("[stories] tools/stories_pack.py could not be read: %s" % e)
        return None


def supported(sp, src, dst):
    """whether stories_pack.py makes `src`'s vehicles for `dst` (VCS -> LCS always has; the other way when it says so)"""
    if (src, dst) == ("vcs", "lcs"):
        return True
    if sp is None:
        return False
    if callable(getattr(sp, "supports", None)):
        try:
            return bool(sp.supports(src, dst))
        except Exception:  # noqa: BLE001 - a tool in the middle of a change: not supported this time
            return False
    return (src, dst) in set(map(tuple, getattr(sp, "DIRECTIONS", ())))


def disc_game(iso):
    """'lcs' / 'vcs' from the disc's file system (GAME.DTZ's place), cheaply; None if not a Stories disc"""
    try:
        import vcs_disc
        img = vcs_disc.IsoImage(iso)
        for game, path in (("lcs", "CHK/PS2/GAME.DTZ"), ("vcs", "PS2/GAME.DTZ")):
            rec = img.find(path)
            if rec and not rec["dir"]:
                return game
    except Exception:  # noqa: BLE001 - unreadable, not an ISO
        return None
    return None


def ini_iso(game):
    """iso= in a game's vcs.ini (build/vcs.ini, build/lcs/vcs.ini: where the launcher writes the disc it checked)"""
    base = tc.BUILD
    folder = base if game == "vcs" or os.environ.get("VCS_BUILD_DIR") else os.path.join(base, game)
    value = tc.ini_value(os.path.join(folder, "vcs.ini"), "iso")
    if not value:
        return None
    if not os.path.isabs(value):
        for b in (folder, base, tc.ROOT):
            if os.path.isfile(os.path.join(b, value)):
                return os.path.join(b, value)
    return value


def data_dir(game):
    """the data folder a game started by the launcher uses (the game's rule: VCS_DATA_DIR, else GTAStoriesRecomp/<GAME>)"""
    if os.environ.get("VCS_DATA_DIR"):
        return os.environ["VCS_DATA_DIR"]
    if tc.IS_WINDOWS:
        base = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "GTAStoriesRecomp")
    elif tc.IS_MACOS:
        base = os.path.expanduser("~/Library/Application Support/GTAStoriesRecomp")
    else:
        xdg = os.environ.get("XDG_DATA_HOME", "")
        base = os.path.join(xdg if xdg.startswith("/") else os.path.expanduser("~/.local/share"), "GTAStoriesRecomp")
    return os.path.join(base, game.upper())


def background_data():
    """Vice City's data folder behind Liberty City (tools/stories.py: <build>/stories/vcs-background)"""
    return os.path.join(tc.BUILD, "stories", "vcs-background")


def targets(mode):
    """[(game, data folder, why)] the packs go to for a start of `mode`"""
    out = []
    if mode in ("lcs", "stories"):
        out.append(("lcs", data_dir("lcs"), "Liberty City Stories"))
    if mode == "vcs":
        out.append(("vcs", data_dir("vcs"), "Vice City Stories"))
    if mode == "stories":
        out.append(("vcs", background_data(), "Vice City behind Liberty City"))
    # one folder for both games (VCS_DATA_DIR: tests) would mix the two games' mods: the second is left out
    seen, kept = set(), []
    for t in out:
        key = os.path.normcase(os.path.abspath(t[1]))
        if key not in seen:
            seen.add(key)
            kept.append(t)
    return kept


def file_hash(names):
    h = hashlib.sha256()
    for name in sorted(set(names)):
        path = os.path.join(HERE, name)
        h.update(name.encode())
        try:
            with open(path, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
        except OSError:
            h.update(b"-")
    return h.hexdigest()[:16]


def disc_key(iso):
    st = os.stat(iso)
    return "%s|%d|%d" % (os.path.normcase(os.path.abspath(iso)), st.st_size, int(st.st_mtime))


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def stamp_for(pack, sp, iso):
    return {"auto": PACK_VERSION, "pack": getattr(sp, "PACK_VERSION", 0) if sp else 0,
            "tools": file_hash(["stories_pack.py", "vcs_disc.py"] + pack.get("tools", [])), "disc": disc_key(iso)}


def why_stale(folder, want):
    """None when the pack in `folder` is current, else what is out of date"""
    if not os.path.isfile(os.path.join(folder, "manifest.json")):
        return "not made yet"
    have = read_json(os.path.join(folder, STAMP))
    if not have:
        return "made before the packs were automatic"
    for key, text in (("auto", "a newer stories_auto.py"), ("pack", "a newer pack version"),
                      ("tools", "the tools that make it changed"), ("disc", "another disc")):
        if have.get(key) != want[key]:
            return text
    return None


def run_pack(pack, iso, data):
    """stories_pack.py with the pack's arguments, its own process (its game's mod manager is chosen at import)"""
    args = [a.replace("{iso}", iso).replace("{data}", data) for a in pack["args"]]
    env = dict(os.environ)
    env.pop("VCS_PROGRESS", None)  # its own events would be taken for ours
    env["VCS_DATA_DIR"] = data  # mods.py (active.txt) and the pack: the same folder
    env["PYTHONIOENCODING"] = "utf-8"
    p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "stories_pack.py")] + args, cwd=tc.ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    for l in lines[-6:]:
        print("  " + l)
    if p.returncode != 0:
        msg = next((l for l in reversed(lines) if "error:" in l.lower()), lines[-1] if lines else "")
        msg = msg[msg.lower().find("error:") + 6:] if "error:" in msg.lower() else msg
        return msg.strip() or "stories_pack.py stopped (exit code %d)" % p.returncode
    return None


def set_enabled(game, data, mod_id, on):
    env = dict(os.environ, VCS_DATA_DIR=data, PYTHONIOENCODING="utf-8")
    env.pop("VCS_PROGRESS", None)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "mods.py"), "--game", game,
                    "enable" if on else "disable", mod_id], cwd=tc.ROOT, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--for", dest="mode", choices=("lcs", "vcs", "stories"), default="stories")
    ap.add_argument("--lcs-iso")
    ap.add_argument("--vcs-iso")
    ap.add_argument("--only", help="kinds, comma-separated (swim, vehicles, ...)")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    if a.json:
        tc.set_json_progress()

    discs, notes = {}, []
    for game, given in (("lcs", a.lcs_iso), ("vcs", a.vcs_iso)):
        iso = given or ini_iso(game)
        if not iso:
            continue
        if not os.path.isfile(iso):
            notes.append("%s disc not found: %s" % (game.upper(), iso))
            continue
        if disc_game(iso) != game:
            notes.append("%s is not the %s disc" % (iso, "Liberty City Stories" if game == "lcs" else "Vice City Stories"))
            continue
        discs[game] = os.path.abspath(iso)

    sp = stories_pack()
    packs = list(PACKS) + [p for p in (getattr(sp, "AUTO_PACKS", None) or []) if isinstance(p, dict)]
    only = {k.strip() for k in a.only.split(",")} if a.only else None
    built, current, failed, skipped, plan = [], [], [], [], []
    for game, data, where in targets(a.mode):
        for pack in packs:
            if pack["game"] != game or (only and pack["kind"] not in only):
                continue
            label = "%s (%s)" % (pack["title"], where) if a.mode == "stories" and game == "vcs" else pack["title"]
            if pack.get("needs") and not supported(sp, *pack["needs"]):
                skipped.append({"mod": pack["mod"], "why": "not supported by tools/stories_pack.py yet"})
                continue
            iso = discs.get(pack["src"])
            if not iso:
                skipped.append({"mod": pack["mod"], "why": "no %s disc" % pack["src"].upper()})
                continue
            folder = os.path.join(data, "mods", pack["mod"])
            want = stamp_for(pack, sp, iso)
            stale = "made again on request" if a.force else why_stale(folder, want)
            if not stale:
                current.append(pack["mod"])
                continue
            plan.append((pack, game, data, folder, want, stale, iso, label))

    if a.check:
        out = {"result": "ok", "make": [{"mod": p[0]["mod"], "data": p[2], "why": p[5]} for p in plan],
               "current": current, "skipped": skipped, "discs": discs, "notes": notes}
        say(**out) if tc.json_progress() else print(json.dumps(out, indent=2))
        return 0

    for i, (pack, game, data, folder, want, stale, iso, label) in enumerate(plan):
        say(stage=pack["mod"] + ":" + game, title="Making " + label)
        say(progress=i, total=len(plan), eta=None, note=label)
        print("[stories] %s: %s -> %s" % (label, stale, folder))
        before = read_json(os.path.join(folder, "manifest.json"))
        old_stamp = read_json(os.path.join(folder, STAMP)) or {}
        # off by the player (a pack this tool made before and found off) stays off; anything else is switched on
        keep_off = bool(before) and not before.get("enabled", True) and bool(old_stamp)
        t0 = time.time()
        # VCS_STORIES_AUTO_FAIL=<kind>: that pack fails without being made (tests of the launcher's note)
        if os.environ.get("VCS_STORIES_AUTO_FAIL") in (pack["kind"], pack["mod"]):
            error = "a test failure (VCS_STORIES_AUTO_FAIL)"
        else:
            error = run_pack(pack, iso, data)
        if error:
            failed.append({"mod": pack["mod"], "title": label, "message": error})
            say(note="%s: %s" % (label, error))
            print("[stories] %s failed: %s" % (label, error))
            continue
        set_enabled(game, data, pack["mod"], not keep_off)
        with open(os.path.join(folder, STAMP), "w", encoding="utf-8") as f:
            json.dump(dict(want, made=int(time.time()), enabled=not keep_off), f, indent=2)
        built.append({"mod": pack["mod"], "title": label, "seconds": round(time.time() - t0, 1), "why": stale,
                      "enabled": not keep_off})
    if plan:
        say(progress=len(plan), total=len(plan), eta=None, note="")
    result = {"result": "ok", "built": built, "current": current, "failed": failed, "skipped": skipped, "notes": notes}
    if tc.json_progress():
        say(**result)
    else:
        for b in built:
            print("[stories] made %s in %.1f s (%s)" % (b["mod"], b["seconds"], "on" if b["enabled"] else "kept off"))
        for c in current:
            print("[stories] %s: current" % c)
        for s in skipped:
            print("[stories] %s: skipped (%s)" % (s["mod"], s["why"]))
        for f in failed:
            print("[stories] %s FAILED: %s" % (f["mod"], f["message"]))
        for n in notes:
            print("[stories] " + n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
